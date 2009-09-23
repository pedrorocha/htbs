-- this should use sha1 hash (40 bits long)

-- logged
INSERT INTO users (login, password, logged_at, logged) VALUES ('guest1', md5('guest1'), CURRENT_TIMESTAMP, 't');
INSERT INTO users (login, password, logged_at, logged) VALUES ('guest2', md5('guest2'), CURRENT_TIMESTAMP, 't');
INSERT INTO users (login, password, logged_at, logged) VALUES ('guest3', md5('guest3'), CURRENT_TIMESTAMP, 't');

-- not logged
INSERT INTO users (login, password) VALUES ('guest4', md5('guest4'));
INSERT INTO users (login, password) VALUES ('guest5', md5('guest5'));


-- giving some credits to logged users..
UPDATE users SET credits = interval '20 minute' WHERE login = 'guest1';
UPDATE users SET credits = interval '60 minute' WHERE login = 'guest2';
UPDATE users SET credits = interval '30 minute' WHERE login = 'guest3';
