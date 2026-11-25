CREATE TABLE `cams` (
  `id` int NOT NULL,
  `name` varchar(30) DEFAULT NULL,
  `snapshot` varchar(250) DEFAULT NULL,
  `last_update` datetime DEFAULT NULL,
  `chat_id` varchar(30) DEFAULT NULL,
  `bot_id` varchar(250) DEFAULT NULL,
  `event_gap` int DEFAULT NULL,
  `owner` varchar(30) DEFAULT NULL,
  `last_external` datetime DEFAULT NULL,
  `video_options` varchar(250) DEFAULT NULL,
  `snapshot_small` varchar(250) DEFAULT NULL,
  `last_cam_event_id` varchar(32) DEFAULT NULL,
  `last_event_id` int DEFAULT NULL,
  `tz` varchar(32) DEFAULT NULL,
  `max_event_days` int DEFAULT NULL,
  `file_path` varchar(250) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE `events` (
  `id` int NOT NULL AUTO_INCREMENT,
  `cam_id` int DEFAULT NULL,
  `event_id` varchar(32) DEFAULT NULL,
  `event_type` varchar(32) DEFAULT NULL,
  `start_time` datetime DEFAULT NULL,
  `end_time` datetime DEFAULT NULL,
  `message_id` varchar(30) DEFAULT NULL,
  `owner` varchar(30) DEFAULT NULL,
  `files_time` datetime DEFAULT NULL,
  `media_time` datetime DEFAULT NULL,
  `chat_time` datetime DEFAULT NULL,
  `cam_event_id` varchar(32) DEFAULT NULL,
  `changed` int DEFAULT NULL,
  `noise` int DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `events_cam_time` (`cam_id`,`start_time`),
  KEY `idx_events_time` (`cam_id`,`start_time`,`end_time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE `files` (
  `event_id` int NOT NULL,
  `type` varchar(30) NOT NULL DEFAULT '',
  `path` varchar(250) NOT NULL DEFAULT '',
  `status` varchar(30) DEFAULT NULL,
  PRIMARY KEY (`event_id`,`path`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE `jobs` (
  `id` int NOT NULL AUTO_INCREMENT,
  `command` varchar(1024) DEFAULT NULL,
  `owner` varchar(30) DEFAULT NULL,
  `status` varchar(30) DEFAULT NULL,
  `update_time` datetime DEFAULT NULL,
  `touch_event_id` int DEFAULT NULL,
  `touch_file_type` varchar(30) DEFAULT NULL,
  `sender` varchar(30) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE `traces` (
  `event_id` int DEFAULT NULL,
  `time` datetime DEFAULT NULL,
  `message` varchar(128) DEFAULT NULL,
  KEY `traces_event_id` (`event_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE `responses` (
  `id` int NOT NULL AUTO_INCREMENT,
  `time` datetime DEFAULT NULL,
  `event_id` int DEFAULT NULL,
  `status_code` varchar(5) DEFAULT NULL,
  `reason` varchar(200) DEFAULT NULL,
  `content` json DEFAULT NULL,
  `command` varchar(64) DEFAULT NULL,
  `parameters` json DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE `timelapses` (
  `cam_id` int DEFAULT NULL,
  `start_time` datetime DEFAULT NULL,
  `end_time` datetime DEFAULT NULL,
  `status` varchar(30) DEFAULT NULL,
  `path` varchar(250) DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
