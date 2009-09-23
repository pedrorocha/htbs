CREATE TABLE users (
	id_user serial NOT NULL,

	-- authentication
	login character varying(128) NOT NULL UNIQUE,
	password character varying(40) NOT NULL,


	-- optional personal information
	name character varying(256),
	email character varying(128),
	address character varying(128),
	phone character varying(64),


	----------------
	-- timestamps
	logged_at timestamp DEFAULT NULL,

	-- here the default should be zero. How do we do this?
	credits time DEFAULT NULL,


	----------------
	-- flags

	-- changing it to true must insert the current 
	-- timestamp into 'logged_at'.
	-- changing to false must update user credits.
	logged boolean DEFAULT false,

	
	----------------
	-- login information

	-- this should also be setted in login to inform which host
	-- the user is logged (IP or DNS name)
	host character varying(512) DEFAULT NULL,

	PRIMARY KEY(id_user)
);
