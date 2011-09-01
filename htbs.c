/*
 * elevator htbs
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/cgroup.h>
#include "blk-cgroup.h"


#define REQ_S(x)		((struct htbs_req*)((x)->elevator_private))->s
#define REQ_F(x)		((struct htbs_req*)((x)->elevator_private))->f

#define DEFAULT_MAX_IDLE_TIME		10		// max time to wait for the next I/O of a queue
#define DEFAULT_MAX_REQS_PER_ROUND	20		// max number of request to serve for an application in a row

#define DEBUG_NEW_QUEUE			1		// show all incomming requests
#define DEBUG_INCOMMING			2		// show all incomming requests
#define DEBUG_DISPATCH			4		// debug dispatched requests
#define DEBUG_TOKEN			8		// debug per queue token count
#define DEBUG_DRIFT			16		// debug timestamp drifts (spare bandwidth)
#define DEBUG_DELAY			32		// debug request delaying (token count exceeded)

#define DEBUG_LEVEL_DEFAULT		0

#define DEBUG(_type, _fmt, ...)	\
		if ((debug_level) & _type) printk(_fmt, __VA_ARGS__)


/* stores actual debug level */
unsigned short int debug_level = DEBUG_LEVEL_DEFAULT;

typedef unsigned long int jiffies_t;

struct htbs_req {
	unsigned long int s;
	unsigned long int f;
};

struct htbs_data {
	struct list_head queue;

	/* per proccess queues */
	struct list_head htbs_groups;

	/* timer to idle queues */
	struct timer_list htbs_timer;

	unsigned int total_reqs;

	unsigned int max_reqs;
	unsigned int max_idle_time;
};

/* per group per device structure */
struct htbs_group {
	/* per process queue list */
	struct list_head list;

	/* request list */
	struct list_head req_list;

	/* number of requests queued here (req_list) */
	unsigned int num_reqs;

	/* task number */
	unsigned int task_pid;

	/* jiffies of the last token update */
	jiffies_t last_updated;

	/* tokens held */
	int tokens;

	/* performance parameters */
	int bw, burst, delay;

	/* htbs specific parameters */
	jiffies_t min_s, max_s, min_f;

	/* number of subsequent requests this group issued */
	int round_reqs;

	/* whether this group is sequential or not */
	int is_sequential;

	sector_t next_sector;

	/* points to cgroup structure */
	struct blkio_cgroup *blkcg;
};

/* TODO - move this into htbs_data*/
/* stores the current queue being served */
struct htbs_group* current_queue = NULL;


/*
 * cgroup functions
 */

/* 
 * this function should come under blk-cgroup.h 
 * in newer kernel versions 
 */
struct blkio_cgroup*
task_blkio_cgroup(struct task_struct *tsk)
{
	return container_of(task_subsys_state(tsk, blkio_subsys_id),
				struct blkio_cgroup, css);
}

struct blkio_cgroup*
htbs_blkio_get_current(void)
{
	return task_blkio_cgroup(current);
}

unsigned int
htbs_blkio_get_weight(struct blkio_cgroup *blkcg)
{
	return blkcg->weight;
}

char*
htbs_blkio_get_path(struct blkio_cgroup *blkcg)
{
	struct blkio_group *blkg;
	struct hlist_node *n;

	/* we definitely should improve this */
	hlist_for_each_entry_rcu(blkg, n, &blkcg->blkg_list, blkcg_node) {
		if (blkg->path)
			return blkg->path;
	}
	return "";
}

/*
 * updating the number of tokens of a group
 */
static void
htbs_update_num_tokens(struct htbs_group *hg)
{
	int new_tokens;

	DEBUG(DEBUG_TOKEN, "[%d] Last updated: [%ld], current: [%ld], elapsed: [%d]\n", 
						hg->task_pid,
						hg->last_updated, 
						jiffies, 
						jiffies_to_msecs(jiffies - hg->last_updated));

	DEBUG(DEBUG_TOKEN, "[%d] Rebuy before: [%d] tokens\n", hg->task_pid, hg->tokens);

	new_tokens = (hg->bw * jiffies_to_msecs(jiffies - hg->last_updated))/10;

	/* there are new tokens*/
	if (new_tokens) {
		hg->tokens += new_tokens;
		hg->last_updated = jiffies;
	}

	if (hg->tokens > ((hg->bw + hg->burst) * 100))
		hg->tokens = (hg->bw + hg->burst) * 100;

	DEBUG(DEBUG_TOKEN, "[%d] Rebuy after: [%d] tokens\n", hg->task_pid, hg->tokens);
}

/*
 * drift back if all tags are in the future
 */
static void 
htbs_adjust_tags(struct htbs_data *hd)
{
	struct htbs_group *cur, *lowest_s;
	struct request *cur_rq;
	jiffies_t drift;

	
	/* no reqs, nothing to do here */
	if (hd->total_reqs == 0)
		return;

	lowest_s = list_entry(hd->htbs_groups.next, struct htbs_group, list);

	/* searching for the queue with the smallest minS */
	list_for_each_entry(cur, &hd->htbs_groups, list) {

		/* empty group */
		if (!cur->num_reqs) continue;

		/* found a queue with a sufficient small minS, quitting */
		if (cur->min_s <= jiffies)
			return;

		if ((cur->min_s < lowest_s->min_s) || !lowest_s->num_reqs)  {
			lowest_s = cur;
		}
	}

	drift = lowest_s->min_s - jiffies;

	DEBUG(DEBUG_DRIFT, "# Drifting [%ld] jiffies from everyone.\n", drift);

	/* if we get here, we must drift all request */
	list_for_each_entry(cur, &hd->htbs_groups, list) {

		/* empty group */
		if (!cur->num_reqs) continue;
	
		cur->min_f -= drift;
		cur->min_s -= drift;
		cur->max_s -= drift;

		/* drifting all requests */
		list_for_each_entry(cur_rq, &cur->req_list, queuelist) {
			REQ_S(cur_rq) -= drift;
			REQ_F(cur_rq) -= drift;
		}

	}
}

/*
 * scheduler run of the queue. Called by htbs_timer timer.
 */
static void
htbs_scheduler_dispatch(unsigned long data)
{
	struct request_queue *q = (struct request_queue *)data;
	struct htbs_data *hd = q->elevator->elevator_data;


	DEBUG(DEBUG_DISPATCH, "[%ld] Scheduler dispatch\n", jiffies);

	/* 
	 * if the timer expired, this function was called,
	 * and there are no request available to serve,
	 * we should service another queue.
	 */
	if (!current_queue->num_reqs)
		current_queue = NULL;

	DEBUG(DEBUG_DISPATCH, "[%ld] Requests left: [%d]\n", jiffies, hd->total_reqs);

	/* run the queue */
	blk_run_queue(q);
}

static void
htbs_schedule_next_dispatch(struct htbs_data *hd)
{
	jiffies_t until;


	until = jiffies + msecs_to_jiffies(hd->max_idle_time);

	DEBUG(DEBUG_DISPATCH, "[%ld] Scheduling next dispatch to ", jiffies);

	/* is there a timer already? */
	if (timer_pending(&hd->htbs_timer)) {
		DEBUG(DEBUG_DISPATCH, "[%ld] already scheduled.. aborted.\n", until);
		return;
	}

	/* set up the timer otherwise */
	if (mod_timer(&hd->htbs_timer, until)) {
		DEBUG(DEBUG_DISPATCH, "[%ld] ERROR setting up timer.\n", until);
		return;
	}
	else
		DEBUG(DEBUG_DISPATCH, "[%ld] OK.\n", until);

	return;
}

/*
 * returns the next queue to serve. This funtion also
 * controls whether to go idle or not.
 */
struct htbs_group*
htbs_select_queue(struct htbs_data* hd, int force)
{
	struct htbs_group *cur, *lowest_f;

	/* all queues are empty */
	if (hd->total_reqs == 0) {
		DEBUG(DEBUG_DISPATCH, "[%ld] No more requests.\n", jiffies);
		return NULL;
	}

	if (current_queue) {

		if ((current_queue->round_reqs < hd->max_reqs) 
			&& (current_queue->is_sequential == 1)
			&& (current_queue->tokens > 0)) {

			if (current_queue->num_reqs)
				return current_queue;

			/* scheduling the next dispatch to wait for the 
			 * next I/O from the same queue 
			 */
			htbs_schedule_next_dispatch(hd);
			return NULL;
		}
	}

	/* if we get here, we must change the current_queue */
	lowest_f = list_entry(hd->htbs_groups.next, struct htbs_group, list);

	/* select queue with the lowest finish time (f) */
	list_for_each_entry(cur, &hd->htbs_groups, list) {

		if (!cur->num_reqs) continue;

		if ((cur->min_f < lowest_f->min_f) || !lowest_f->num_reqs)  {
			lowest_f = cur;
		}
	}
	
	/* uhdating current queue */
	current_queue = lowest_f;
	current_queue->round_reqs = 0;

	return lowest_f;
}

/*
 * main dispatch function
 */
static int 
htbs_dispatch(struct request_queue *q, int force)
{
	struct htbs_data *hd = q->elevator->elevator_data;
	struct htbs_group *next_q;
	struct request *rq;


	DEBUG(DEBUG_DISPATCH, "[%ld] Dispatching\n", jiffies);

	/* search for the next queue to serve */
	next_q = htbs_select_queue(hd, force);
	if (!next_q) {
		DEBUG(DEBUG_DISPATCH, "[%ld] Nothing to dispatch now\n", jiffies);
		return 0;
	}

	/* take off the first request on the queue */
	rq = list_entry(next_q->req_list.next, struct request, queuelist);
	list_del_init(&rq->queuelist);

	/* uhdating min_s */
	next_q->min_s = REQ_S(rq);

	/* uhdating min_f */
	if (next_q->num_reqs > 1) {
		next_q->min_f = REQ_F(list_entry(next_q->req_list.next, struct request, queuelist));
	}

	next_q->round_reqs++;
	next_q->num_reqs--;
	hd->total_reqs--;

	/* whether we should consider this queue sequential or not */
	if (next_q->next_sector && next_q->is_sequential)

		if (next_q->next_sector != rq->bio->bi_sector) {
			DEBUG(DEBUG_DISPATCH, "[%ld][%d] Marking this queue as non sequential\n", jiffies, next_q->task_pid);
			next_q->is_sequential = 0;
		}

	if (next_q->is_sequential)
		next_q->next_sector = rq->bio->bi_sector + (rq->bio->bi_size / 512);

	DEBUG(DEBUG_DISPATCH, "[%ld][%s] Dispatch request (s: [%ld], f: [%ld], rr: [%d], sector: [%d], size: [%d], next_sector[%d])\n", 
						jiffies,
						htbs_blkio_get_path(next_q->blkcg),
						REQ_S(rq),
						REQ_F(rq),
						next_q->round_reqs,
						(int)rq->bio->bi_sector,
						(int)rq->bio->bi_size,
						(int)next_q->next_sector);

	elv_dispatch_sort(q, rq);
	return 1;
}


static int
htbs_req_has_priority(struct htbs_data *hd, struct htbs_group *hg)
{

	if ((current_queue == hg) && 
		(!hg->num_reqs) && 
		(current_queue->round_reqs < hd->max_reqs)) {

    	if (timer_pending(&hd->htbs_timer)) {
			mod_timer(&hd->htbs_timer, 0);
		}
		return 1;
	}

	return 0;
}

static void 
htbs_add_request(struct request_queue *q, struct request *rq)
{
	struct htbs_data *hd = q->elevator->elevator_data;
	struct htbs_group *cur;
	struct blkio_cgroup *blkcg;
	int delay_offset;


	blkcg = htbs_blkio_get_current();

	/* find the right queue */
	list_for_each_entry(cur, &hd->htbs_groups, list) {

		if (cur->blkcg == blkcg) {
			break;
		}
	}

	htbs_update_num_tokens(cur);

	htbs_adjust_tags(hd);

	/* computing tags */
	if (cur->tokens < 1) {
		DEBUG(DEBUG_DELAY, "[%ld][%d] Delaying request\n", jiffies, cur->task_pid);
		delay_offset = msecs_to_jiffies(1000/cur->bw);

		/* since we cannot work with float here, we'll assume
		 * that the minimum delay a queue should suffer for
		 * sending more than it's contractual guarantees is 1 ms.
		 */
		if (!delay_offset)
			delay_offset = 1;

		REQ_S(rq) = cur->max_s + delay_offset;
	}
	/* queue have enough tokens */
	else {
	
		/*
		 * if this queue is being served, treat this request
		 * like it was created in the same timestamp as the
		 * previous one.
		 */
		if (htbs_req_has_priority(hd, cur))
			REQ_S(rq) = cur->min_s;
		else
			REQ_S(rq) = jiffies;

	}
	REQ_F(rq) = REQ_S(rq) + msecs_to_jiffies(cur->delay);


	/* now, update per queue control timestamps */

	/* queue was empty */
	if (cur->num_reqs == 0) {
		cur->min_s = cur->max_s = REQ_S(rq);
		cur->min_f = REQ_F(rq);
	} 
	else {

		/* uhdating timestamps */
		if (REQ_S(rq) < cur->min_s)
			cur->min_s = REQ_S(rq); 

		if (REQ_S(rq) > cur->max_s)
			cur->max_s = REQ_S(rq); 

		if (REQ_F(rq) < cur->min_f)
			cur->min_f = REQ_F(rq);
	}

	/* append request to list */
	list_add_tail(&rq->queuelist, &cur->req_list);
	cur->num_reqs++;
	hd->total_reqs++;

	DEBUG(DEBUG_INCOMMING, "[%ld][%s] Add request (s: [%ld], f: [%ld], sector: [%d], size: [%d])\n", 
						jiffies,
						htbs_blkio_get_path(cur->blkcg),
						REQ_S(rq),
						REQ_F(rq),
						(int)rq->bio->bi_sector,
						(int)rq->bio->bi_size);

	DEBUG(DEBUG_TOKEN, "[%ld][%d] Timestamps updated: (min_s: [%ld], max_s: [%ld], min_f: [%ld])\n",
						jiffies,
						cur->task_pid,
						cur->min_s,
						cur->max_s,
						cur->min_f);
		
	/* decreasing token number */
	cur->tokens -= (1 * 100);

	DEBUG(DEBUG_TOKEN, "[%ld][%d] Tokens left: [%d]\n", jiffies, cur->task_pid, cur->tokens);
}

/*
 * creates a new htbs group
 */
static struct htbs_group*
htbs_new_group(struct request_queue *q, struct blkio_cgroup *blkcg)
{
	struct htbs_group *new_hg;
	unsigned int weight;


	new_hg = kmalloc_node(sizeof(*new_hg), GFP_KERNEL, q->node);
	new_hg->task_pid = current->pid;
	new_hg->num_reqs = 0;
	new_hg->last_updated = jiffies;
	new_hg->round_reqs = 0;
	new_hg->is_sequential = 1;
	new_hg->next_sector = 0;
	new_hg->blkcg = blkcg;

	weight = htbs_blkio_get_weight(blkcg);

	new_hg->tokens = weight * 100;
	new_hg->bw = weight;
	new_hg->burst = 0;
	new_hg->delay = 100;
	DEBUG(DEBUG_NEW_QUEUE, "[%ld][%d] Creating new queue with cgroup [%s] ([%d] [%d] [%d])\n", 
						jiffies, 
						current->pid,
						htbs_blkio_get_path(blkcg),
						new_hg->bw,
						new_hg->burst,
						new_hg->delay);

	return new_hg;
}


/*
 * check whether the task has its own queue or not. If not,
 * create it.
 */
static int
htbs_set_request(struct request_queue *q, struct request *rq, gfp_t gfp_mask)
{
	struct htbs_data *hd = q->elevator->elevator_data;
	struct htbs_group *new_hg, *cur;
	struct htbs_req *preq;
	struct blkio_cgroup *blkcg;


	DEBUG(DEBUG_INCOMMING, "[%ld][%d] Setting up a request\n", jiffies, current->pid);

	/* creating fair queueing parameters */
	preq = kmalloc_node(sizeof(*preq), GFP_KERNEL, q->node);
	rq->elevator_private = preq;

	/* getting current cgroup */
	blkcg = htbs_blkio_get_current();

	list_for_each_entry(cur, &hd->htbs_groups, list) {

		/* there is a queue already */
		if (cur->blkcg == blkcg) {
			return 0;
		}
	}

	/* creates a new htbs group*/
	new_hg = htbs_new_group(q, blkcg);

	INIT_LIST_HEAD(&new_hg->req_list);
	list_add_tail(&new_hg->list, &hd->htbs_groups);

	return 0;
}

/* deallocating request specific data */
static void
htbs_put_request(struct request *rq)
{
	kfree(rq->elevator_private);
}


/* returns whether the queue is empty or not */
static int 
htbs_queue_empty(struct request_queue *q)
{
	struct htbs_data *hd = q->elevator->elevator_data;

	if (current_queue)
		return !current_queue->num_reqs;
	return !hd->total_reqs;
}

/*
 * Initialize scheduler stuff
 */
static void *htbs_init_queue(struct request_queue *q)
{
	struct htbs_data *hd;

	hd = kmalloc_node(sizeof(*hd), GFP_KERNEL, q->node);
	if (!hd)
		return NULL;

	/* initializing queues */
	INIT_LIST_HEAD(&hd->queue);
	INIT_LIST_HEAD(&hd->htbs_groups);
	hd->total_reqs = 0;
	hd->max_reqs = DEFAULT_MAX_REQS_PER_ROUND;
	hd->max_idle_time = DEFAULT_MAX_IDLE_TIME;

	setup_timer(&hd->htbs_timer, htbs_scheduler_dispatch, (unsigned long)q);

	return hd;
}

static void htbs_exit_queue(struct elevator_queue *e)
{
	struct htbs_data *hd = e->elevator_data;

	del_timer(&hd->htbs_timer);
	BUG_ON(!list_empty(&hd->queue));
	kfree(hd);
}


/*
 * sysfs functions
 */
static ssize_t 
htbs_max_reqs_show(struct elevator_queue *e, char *page)
{
    struct htbs_data *hd = e->elevator_data;

    return sprintf(page, "%d\n", hd->max_reqs);
}

static ssize_t 
htbs_max_reqs_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct htbs_data *hd = e->elevator_data;
	char *p = (char *)page;
	int value = simple_strtol(p, &p, 10);

	/* checking input */
	if (value < 1) 
		value = 1;
	else if (value > UINT_MAX)
		value = UINT_MAX;

	hd->max_reqs = (unsigned int)value;
	printk("htbs: max_reqs changed to %d\n", hd->max_reqs);
	return count;
}

static ssize_t 
htbs_debug_level_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%hd\n", debug_level);
}

static ssize_t 
htbs_debug_level_store(struct elevator_queue *e, const char *page, size_t count)
{
	char *p = (char *)page;
	int value = simple_strtol(p, &p, 10);

	/* checking input */
	if (value < 0) 
		value = 0;
	else if (value > 63)
		value = 63;

	debug_level = (unsigned short int)value;
	printk("htbs: debug_level changed to %hd\n", debug_level);

	return count;
}

static ssize_t 
htbs_cleanup_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "0\n");
}

static ssize_t 
htbs_cleanup_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct htbs_data *hd = e->elevator_data;
	struct htbs_group *cur;

	printk("htbs: do cleanup\n");

	/* find the right queue */
	list_for_each_entry(cur, &hd->htbs_groups, list) {

		/* TODO: here we should also reset queue and per-request
		 * tags to a default value.
		 */
		cur->last_updated = jiffies;
		cur->round_reqs = 0;
		cur->is_sequential = 1;
		cur->next_sector = 0;
		cur->tokens = htbs_blkio_get_weight(cur->blkcg) * 100;
	}
	return count;
}

/* 
 * this function just prints the cgroup hierarchy. we must 
 * remove it sometime. 
 */
static ssize_t 
htbs_cgroup_show(struct elevator_queue *e, char *page)
{
	struct blkio_cgroup *blkcg = task_blkio_cgroup(current);
	struct blkio_group *blkg;
	struct hlist_node *n;


	if (blkcg == &blkio_root_cgroup)
		printk("htbs: cgroup root [%d]\n", blkcg->weight);
	else
		printk("htbs: cgroup non-root [%d]\n", blkcg->weight);

	hlist_for_each_entry_rcu(blkg, n, &blkcg->blkg_list, blkcg_node) {
		printk("htbs: [%s] [%d]\n", blkg->path, blkg->blkcg_id);
		if (blkg->dev) {
			printk("htbs: tem dev\n");
		}
	}

	return sprintf(page, "0\n");
}

static ssize_t 
htbs_cgroup_store(struct elevator_queue *e, const char *page, size_t count)
{
	return count;
}


/*
 * Attrs acessible via sysfs
 */
static struct elv_fs_entry htbs_attrs[] = {
	__ATTR(max_reqs, S_IRUGO|S_IWUSR, htbs_max_reqs_show, htbs_max_reqs_store),
	__ATTR(debug_level, S_IRUGO|S_IWUSR, htbs_debug_level_show, htbs_debug_level_store),
	__ATTR(cleanup, S_IRUGO|S_IWUSR, htbs_cleanup_show, htbs_cleanup_store),
	__ATTR(test_cgroup, S_IRUGO|S_IWUSR, htbs_cgroup_show, htbs_cgroup_store),
	__ATTR_NULL,
};

/*
 * Registering our operations.
 */
static struct elevator_type elevator_htbs = {
	.ops = {
		.elevator_dispatch_fn		= htbs_dispatch,
		.elevator_add_req_fn		= htbs_add_request,
		.elevator_set_req_fn		= htbs_set_request,
		.elevator_put_req_fn		= htbs_put_request,
		.elevator_queue_empty_fn	= htbs_queue_empty,
		.elevator_init_fn		= htbs_init_queue,
		.elevator_exit_fn		= htbs_exit_queue,
	},
	.elevator_attrs = htbs_attrs,
	.elevator_name = "htbs",
	.elevator_owner = THIS_MODULE,
};

/*
 * Kernel module stuff here
 */
static int __init htbs_init(void)
{
	elv_register(&elevator_htbs);
	printk("htbs: loaded\n");

	return 0;
}

static void __exit htbs_exit(void)
{
	elv_unregister(&elevator_htbs);
	printk("htbs: unloading. Bye!\n");
}

module_init(htbs_init);
module_exit(htbs_exit);

MODULE_AUTHOR("Pedro Eugenio Rocha");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("The HTBS I/O scheduler");
