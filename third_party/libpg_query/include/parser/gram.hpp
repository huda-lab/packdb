/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton interface for Bison's Yacc-like parsers in C

   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     IDENT = 258,
     FCONST = 259,
     SCONST = 260,
     BCONST = 261,
     XCONST = 262,
     Op = 263,
     ICONST = 264,
     PARAM = 265,
     TYPECAST = 266,
     DOT_DOT = 267,
     COLON_EQUALS = 268,
     EQUALS_GREATER = 269,
     INTEGER_DIVISION = 270,
     POWER_OF = 271,
     LAMBDA_ARROW = 272,
     DOUBLE_ARROW = 273,
     LESS_EQUALS = 274,
     GREATER_EQUALS = 275,
     NOT_EQUALS = 276,
     ABORT_P = 277,
     ABSOLUTE_P = 278,
     ACCESS = 279,
     ACTION = 280,
     ADD_P = 281,
     ADMIN = 282,
     AFTER = 283,
     AGGREGATE = 284,
     ALL = 285,
     ALSO = 286,
     ALTER = 287,
     ALWAYS = 288,
     ANALYSE = 289,
     ANALYZE = 290,
     AND = 291,
     ANTI = 292,
     ANY = 293,
     ARRAY = 294,
     AS = 295,
     ASC_P = 296,
     ASOF = 297,
     ASSERTION = 298,
     ASSIGNMENT = 299,
     ASYMMETRIC = 300,
     AT = 301,
     ATTACH = 302,
     ATTRIBUTE = 303,
     AUTHORIZATION = 304,
     BACKWARD = 305,
     BEFORE = 306,
     BEGIN_P = 307,
     BETWEEN = 308,
     BIGINT = 309,
     BINARY = 310,
     BIT = 311,
     BOOLEAN_P = 312,
     BOTH = 313,
     BY = 314,
     CACHE = 315,
     CALL_P = 316,
     CALLED = 317,
     CASCADE = 318,
     CASCADED = 319,
     CASE = 320,
     CAST = 321,
     CATALOG_P = 322,
     CENTURIES_P = 323,
     CENTURY_P = 324,
     CHAIN = 325,
     CHAR_P = 326,
     CHARACTER = 327,
     CHARACTERISTICS = 328,
     CHECK_P = 329,
     CHECKPOINT = 330,
     CLASS = 331,
     CLOSE = 332,
     CLUSTER = 333,
     COALESCE = 334,
     COLLATE = 335,
     COLLATION = 336,
     COLUMN = 337,
     COLUMNS = 338,
     COMMENT = 339,
     COMMENTS = 340,
     COMMIT = 341,
     COMMITTED = 342,
     COMPRESSION = 343,
     CONCURRENTLY = 344,
     CONFIGURATION = 345,
     CONFLICT = 346,
     CONNECTION = 347,
     CONSTRAINT = 348,
     CONSTRAINTS = 349,
     CONTENT_P = 350,
     CONTINUE_P = 351,
     CONVERSION_P = 352,
     COPY = 353,
     COST = 354,
     CREATE_P = 355,
     CROSS = 356,
     CSV = 357,
     CUBE = 358,
     CURRENT_P = 359,
     CURSOR = 360,
     CYCLE = 361,
     DATA_P = 362,
     DATABASE = 363,
     DAY_P = 364,
     DAYS_P = 365,
     DEALLOCATE = 366,
     DEC = 367,
     DECADE_P = 368,
     DECADES_P = 369,
     DECIMAL_P = 370,
     DECLARE = 371,
     DEFAULT = 372,
     DEFAULTS = 373,
     DEFERRABLE = 374,
     DEFERRED = 375,
     DEFINER = 376,
     DELETE_P = 377,
     DELIMITER = 378,
     DELIMITERS = 379,
     DEPENDS = 380,
     DESC_P = 381,
     DESCRIBE = 382,
     DETACH = 383,
     DICTIONARY = 384,
     DISABLE_P = 385,
     DISCARD = 386,
     DISTINCT = 387,
     DO = 388,
     DOCUMENT_P = 389,
     DOMAIN_P = 390,
     DOUBLE_P = 391,
     DROP = 392,
     EACH = 393,
     ELSE = 394,
     ENABLE_P = 395,
     ENCODING = 396,
     ENCRYPTED = 397,
     END_P = 398,
     ENUM_P = 399,
     ESCAPE = 400,
     EVENT = 401,
     EXCEPT = 402,
     EXCLUDE = 403,
     EXCLUDING = 404,
     EXCLUSIVE = 405,
     EXECUTE = 406,
     EXISTS = 407,
     EXPLAIN = 408,
     EXPORT_P = 409,
     EXPORT_STATE = 410,
     EXTENSION = 411,
     EXTENSIONS = 412,
     EXTERNAL = 413,
     EXTRACT = 414,
     FALSE_P = 415,
     FAMILY = 416,
     FETCH = 417,
     FILTER = 418,
     FIRST_P = 419,
     FLOAT_P = 420,
     FOLLOWING = 421,
     FOR = 422,
     FORCE = 423,
     FOREIGN = 424,
     FORWARD = 425,
     FREEZE = 426,
     FROM = 427,
     FULL = 428,
     FUNCTION = 429,
     FUNCTIONS = 430,
     GENERATED = 431,
     GLOB = 432,
     GLOBAL = 433,
     GRANT = 434,
     GRANTED = 435,
     GROUP_P = 436,
     GROUPING = 437,
     GROUPING_ID = 438,
     GROUPS = 439,
     HANDLER = 440,
     HAVING = 441,
     HEADER_P = 442,
     HOLD = 443,
     HOUR_P = 444,
     HOURS_P = 445,
     IDENTITY_P = 446,
     IF_P = 447,
     IGNORE_P = 448,
     ILIKE = 449,
     IMMEDIATE = 450,
     IMMUTABLE = 451,
     IMPLICIT_P = 452,
     IMPORT_P = 453,
     IN_P = 454,
     INCLUDE_P = 455,
     INCLUDING = 456,
     INCREMENT = 457,
     INDEX = 458,
     INDEXES = 459,
     INHERIT = 460,
     INHERITS = 461,
     INITIALLY = 462,
     INLINE_P = 463,
     INNER_P = 464,
     INOUT = 465,
     INPUT_P = 466,
     INSENSITIVE = 467,
     INSERT = 468,
     INSTALL = 469,
     INSTEAD = 470,
     INT_P = 471,
     INTEGER = 472,
     INTERSECT = 473,
     INTERVAL = 474,
     INTO = 475,
     INVOKER = 476,
     IS = 477,
     ISNULL = 478,
     ISOLATION = 479,
     JOIN = 480,
     JSON = 481,
     KEY = 482,
     LABEL = 483,
     LANGUAGE = 484,
     LARGE_P = 485,
     LAST_P = 486,
     LATERAL_P = 487,
     LEADING = 488,
     LEAKPROOF = 489,
     LEFT = 490,
     LEVEL = 491,
     LIKE = 492,
     LIMIT = 493,
     LISTEN = 494,
     LOAD = 495,
     LOCAL = 496,
     LOCATION = 497,
     LOCK_P = 498,
     LOCKED = 499,
     LOGGED = 500,
     MACRO = 501,
     MAP = 502,
     MAPPING = 503,
     MATCH = 504,
     MATERIALIZED = 505,
     MAXIMIZE = 506,
     MAXVALUE = 507,
     METHOD = 508,
     MICROSECOND_P = 509,
     MICROSECONDS_P = 510,
     MILLENNIA_P = 511,
     MILLENNIUM_P = 512,
     MILLISECOND_P = 513,
     MILLISECONDS_P = 514,
     MINIMIZE = 515,
     MINUTE_P = 516,
     MINUTES_P = 517,
     MINVALUE = 518,
     MODE = 519,
     MONTH_P = 520,
     MONTHS_P = 521,
     MOVE = 522,
     NAME_P = 523,
     NAMES = 524,
     NATIONAL = 525,
     NATURAL = 526,
     NCHAR = 527,
     NEW = 528,
     NEXT = 529,
     NO = 530,
     NONE = 531,
     NOT = 532,
     NOTHING = 533,
     NOTIFY = 534,
     NOTNULL = 535,
     NOWAIT = 536,
     NULL_P = 537,
     NULLIF = 538,
     NULLS_P = 539,
     NUMERIC = 540,
     OBJECT_P = 541,
     OF = 542,
     OFF = 543,
     OFFSET = 544,
     OIDS = 545,
     OLD = 546,
     ON = 547,
     ONLY = 548,
     OPERATOR = 549,
     OPTION = 550,
     OPTIONS = 551,
     OR = 552,
     ORDER = 553,
     ORDINALITY = 554,
     OTHERS = 555,
     OUT_P = 556,
     OUTER_P = 557,
     OVER = 558,
     OVERLAPS = 559,
     OVERLAY = 560,
     OVERRIDING = 561,
     OWNED = 562,
     OWNER = 563,
     PACKAGE = 564,
     PARALLEL = 565,
     PARSER = 566,
     PARTIAL = 567,
     PARTITION = 568,
     PASSING = 569,
     PASSWORD = 570,
     PERCENT = 571,
     PERSISTENT = 572,
     PIVOT = 573,
     PIVOT_LONGER = 574,
     PIVOT_WIDER = 575,
     PLACING = 576,
     PLANS = 577,
     POLICY = 578,
     POSITION = 579,
     POSITIONAL = 580,
     PRAGMA_P = 581,
     PRECEDING = 582,
     PRECISION = 583,
     PREPARE = 584,
     PREPARED = 585,
     PRESERVE = 586,
     PRIMARY = 587,
     PRIOR = 588,
     PRIVILEGES = 589,
     PROCEDURAL = 590,
     PROCEDURE = 591,
     PROGRAM = 592,
     PUBLICATION = 593,
     QUALIFY = 594,
     QUARTER_P = 595,
     QUARTERS_P = 596,
     QUOTE = 597,
     RANGE = 598,
     READ_P = 599,
     REAL = 600,
     REASSIGN = 601,
     RECHECK = 602,
     RECURSIVE = 603,
     REF = 604,
     REFERENCES = 605,
     REFERENCING = 606,
     REFRESH = 607,
     REINDEX = 608,
     RELATIVE_P = 609,
     RELEASE = 610,
     RENAME = 611,
     REPEAT = 612,
     REPEATABLE = 613,
     REPLACE = 614,
     REPLICA = 615,
     RESET = 616,
     RESPECT_P = 617,
     RESTART = 618,
     RESTRICT = 619,
     RETURNING = 620,
     RETURNS = 621,
     REVOKE = 622,
     RIGHT = 623,
     ROLE = 624,
     ROLLBACK = 625,
     ROLLUP = 626,
     ROW = 627,
     ROWS = 628,
     RULE = 629,
     SAMPLE = 630,
     SAVEPOINT = 631,
     SCHEMA = 632,
     SCHEMAS = 633,
     SCOPE = 634,
     SCROLL = 635,
     SEARCH = 636,
     SECOND_P = 637,
     SECONDS_P = 638,
     SECRET = 639,
     SECURITY = 640,
     SELECT = 641,
     SEMI = 642,
     SEQUENCE = 643,
     SEQUENCES = 644,
     SERIALIZABLE = 645,
     SERVER = 646,
     SESSION = 647,
     SET = 648,
     SETOF = 649,
     SETS = 650,
     SHARE = 651,
     SHOW = 652,
     SIMILAR = 653,
     SIMPLE = 654,
     SKIP = 655,
     SMALLINT = 656,
     SNAPSHOT = 657,
     SOME = 658,
     SQL_P = 659,
     STABLE = 660,
     STANDALONE_P = 661,
     START = 662,
     STATEMENT = 663,
     STATISTICS = 664,
     STDIN = 665,
     STDOUT = 666,
     STORAGE = 667,
     STORED = 668,
     STRICT_P = 669,
     STRIP_P = 670,
     STRUCT = 671,
     SUBSCRIPTION = 672,
     SUBSTRING = 673,
     SUCH = 674,
     SUMMARIZE = 675,
     SYMMETRIC = 676,
     SYSID = 677,
     SYSTEM_P = 678,
     TABLE = 679,
     TABLES = 680,
     TABLESAMPLE = 681,
     TABLESPACE = 682,
     TEMP = 683,
     TEMPLATE = 684,
     TEMPORARY = 685,
     TEXT_P = 686,
     THAT = 687,
     THEN = 688,
     TIES = 689,
     TIME = 690,
     TIMESTAMP = 691,
     TO = 692,
     TRAILING = 693,
     TRANSACTION = 694,
     TRANSFORM = 695,
     TREAT = 696,
     TRIGGER = 697,
     TRIM = 698,
     TRUE_P = 699,
     TRUNCATE = 700,
     TRUSTED = 701,
     TRY_CAST = 702,
     TYPE_P = 703,
     TYPES_P = 704,
     UNBOUNDED = 705,
     UNCOMMITTED = 706,
     UNENCRYPTED = 707,
     UNION = 708,
     UNIQUE = 709,
     UNKNOWN = 710,
     UNLISTEN = 711,
     UNLOGGED = 712,
     UNPIVOT = 713,
     UNTIL = 714,
     UPDATE = 715,
     USE_P = 716,
     USER = 717,
     USING = 718,
     VACUUM = 719,
     VALID = 720,
     VALIDATE = 721,
     VALIDATOR = 722,
     VALUE_P = 723,
     VALUES = 724,
     VARCHAR = 725,
     VARIABLE_P = 726,
     VARIADIC = 727,
     VARYING = 728,
     VERBOSE = 729,
     VERSION_P = 730,
     VIEW = 731,
     VIEWS = 732,
     VIRTUAL = 733,
     VOLATILE = 734,
     WEEK_P = 735,
     WEEKS_P = 736,
     WHEN = 737,
     WHERE = 738,
     WHITESPACE_P = 739,
     WINDOW = 740,
     WITH = 741,
     WITHIN = 742,
     WITHOUT = 743,
     WORK = 744,
     WRAPPER = 745,
     WRITE_P = 746,
     XML_P = 747,
     XMLATTRIBUTES = 748,
     XMLCONCAT = 749,
     XMLELEMENT = 750,
     XMLEXISTS = 751,
     XMLFOREST = 752,
     XMLNAMESPACES = 753,
     XMLPARSE = 754,
     XMLPI = 755,
     XMLROOT = 756,
     XMLSERIALIZE = 757,
     XMLTABLE = 758,
     YEAR_P = 759,
     YEARS_P = 760,
     YES_P = 761,
     ZONE = 762,
     NOT_LA = 763,
     NULLS_LA = 764,
     WITH_LA = 765,
     POSTFIXOP = 766,
     UMINUS = 767
   };
#endif
/* Tokens.  */
#define IDENT 258
#define FCONST 259
#define SCONST 260
#define BCONST 261
#define XCONST 262
#define Op 263
#define ICONST 264
#define PARAM 265
#define TYPECAST 266
#define DOT_DOT 267
#define COLON_EQUALS 268
#define EQUALS_GREATER 269
#define INTEGER_DIVISION 270
#define POWER_OF 271
#define LAMBDA_ARROW 272
#define DOUBLE_ARROW 273
#define LESS_EQUALS 274
#define GREATER_EQUALS 275
#define NOT_EQUALS 276
#define ABORT_P 277
#define ABSOLUTE_P 278
#define ACCESS 279
#define ACTION 280
#define ADD_P 281
#define ADMIN 282
#define AFTER 283
#define AGGREGATE 284
#define ALL 285
#define ALSO 286
#define ALTER 287
#define ALWAYS 288
#define ANALYSE 289
#define ANALYZE 290
#define AND 291
#define ANTI 292
#define ANY 293
#define ARRAY 294
#define AS 295
#define ASC_P 296
#define ASOF 297
#define ASSERTION 298
#define ASSIGNMENT 299
#define ASYMMETRIC 300
#define AT 301
#define ATTACH 302
#define ATTRIBUTE 303
#define AUTHORIZATION 304
#define BACKWARD 305
#define BEFORE 306
#define BEGIN_P 307
#define BETWEEN 308
#define BIGINT 309
#define BINARY 310
#define BIT 311
#define BOOLEAN_P 312
#define BOTH 313
#define BY 314
#define CACHE 315
#define CALL_P 316
#define CALLED 317
#define CASCADE 318
#define CASCADED 319
#define CASE 320
#define CAST 321
#define CATALOG_P 322
#define CENTURIES_P 323
#define CENTURY_P 324
#define CHAIN 325
#define CHAR_P 326
#define CHARACTER 327
#define CHARACTERISTICS 328
#define CHECK_P 329
#define CHECKPOINT 330
#define CLASS 331
#define CLOSE 332
#define CLUSTER 333
#define COALESCE 334
#define COLLATE 335
#define COLLATION 336
#define COLUMN 337
#define COLUMNS 338
#define COMMENT 339
#define COMMENTS 340
#define COMMIT 341
#define COMMITTED 342
#define COMPRESSION 343
#define CONCURRENTLY 344
#define CONFIGURATION 345
#define CONFLICT 346
#define CONNECTION 347
#define CONSTRAINT 348
#define CONSTRAINTS 349
#define CONTENT_P 350
#define CONTINUE_P 351
#define CONVERSION_P 352
#define COPY 353
#define COST 354
#define CREATE_P 355
#define CROSS 356
#define CSV 357
#define CUBE 358
#define CURRENT_P 359
#define CURSOR 360
#define CYCLE 361
#define DATA_P 362
#define DATABASE 363
#define DAY_P 364
#define DAYS_P 365
#define DEALLOCATE 366
#define DEC 367
#define DECADE_P 368
#define DECADES_P 369
#define DECIMAL_P 370
#define DECLARE 371
#define DEFAULT 372
#define DEFAULTS 373
#define DEFERRABLE 374
#define DEFERRED 375
#define DEFINER 376
#define DELETE_P 377
#define DELIMITER 378
#define DELIMITERS 379
#define DEPENDS 380
#define DESC_P 381
#define DESCRIBE 382
#define DETACH 383
#define DICTIONARY 384
#define DISABLE_P 385
#define DISCARD 386
#define DISTINCT 387
#define DO 388
#define DOCUMENT_P 389
#define DOMAIN_P 390
#define DOUBLE_P 391
#define DROP 392
#define EACH 393
#define ELSE 394
#define ENABLE_P 395
#define ENCODING 396
#define ENCRYPTED 397
#define END_P 398
#define ENUM_P 399
#define ESCAPE 400
#define EVENT 401
#define EXCEPT 402
#define EXCLUDE 403
#define EXCLUDING 404
#define EXCLUSIVE 405
#define EXECUTE 406
#define EXISTS 407
#define EXPLAIN 408
#define EXPORT_P 409
#define EXPORT_STATE 410
#define EXTENSION 411
#define EXTENSIONS 412
#define EXTERNAL 413
#define EXTRACT 414
#define FALSE_P 415
#define FAMILY 416
#define FETCH 417
#define FILTER 418
#define FIRST_P 419
#define FLOAT_P 420
#define FOLLOWING 421
#define FOR 422
#define FORCE 423
#define FOREIGN 424
#define FORWARD 425
#define FREEZE 426
#define FROM 427
#define FULL 428
#define FUNCTION 429
#define FUNCTIONS 430
#define GENERATED 431
#define GLOB 432
#define GLOBAL 433
#define GRANT 434
#define GRANTED 435
#define GROUP_P 436
#define GROUPING 437
#define GROUPING_ID 438
#define GROUPS 439
#define HANDLER 440
#define HAVING 441
#define HEADER_P 442
#define HOLD 443
#define HOUR_P 444
#define HOURS_P 445
#define IDENTITY_P 446
#define IF_P 447
#define IGNORE_P 448
#define ILIKE 449
#define IMMEDIATE 450
#define IMMUTABLE 451
#define IMPLICIT_P 452
#define IMPORT_P 453
#define IN_P 454
#define INCLUDE_P 455
#define INCLUDING 456
#define INCREMENT 457
#define INDEX 458
#define INDEXES 459
#define INHERIT 460
#define INHERITS 461
#define INITIALLY 462
#define INLINE_P 463
#define INNER_P 464
#define INOUT 465
#define INPUT_P 466
#define INSENSITIVE 467
#define INSERT 468
#define INSTALL 469
#define INSTEAD 470
#define INT_P 471
#define INTEGER 472
#define INTERSECT 473
#define INTERVAL 474
#define INTO 475
#define INVOKER 476
#define IS 477
#define ISNULL 478
#define ISOLATION 479
#define JOIN 480
#define JSON 481
#define KEY 482
#define LABEL 483
#define LANGUAGE 484
#define LARGE_P 485
#define LAST_P 486
#define LATERAL_P 487
#define LEADING 488
#define LEAKPROOF 489
#define LEFT 490
#define LEVEL 491
#define LIKE 492
#define LIMIT 493
#define LISTEN 494
#define LOAD 495
#define LOCAL 496
#define LOCATION 497
#define LOCK_P 498
#define LOCKED 499
#define LOGGED 500
#define MACRO 501
#define MAP 502
#define MAPPING 503
#define MATCH 504
#define MATERIALIZED 505
#define MAXIMIZE 506
#define MAXVALUE 507
#define METHOD 508
#define MICROSECOND_P 509
#define MICROSECONDS_P 510
#define MILLENNIA_P 511
#define MILLENNIUM_P 512
#define MILLISECOND_P 513
#define MILLISECONDS_P 514
#define MINIMIZE 515
#define MINUTE_P 516
#define MINUTES_P 517
#define MINVALUE 518
#define MODE 519
#define MONTH_P 520
#define MONTHS_P 521
#define MOVE 522
#define NAME_P 523
#define NAMES 524
#define NATIONAL 525
#define NATURAL 526
#define NCHAR 527
#define NEW 528
#define NEXT 529
#define NO 530
#define NONE 531
#define NOT 532
#define NOTHING 533
#define NOTIFY 534
#define NOTNULL 535
#define NOWAIT 536
#define NULL_P 537
#define NULLIF 538
#define NULLS_P 539
#define NUMERIC 540
#define OBJECT_P 541
#define OF 542
#define OFF 543
#define OFFSET 544
#define OIDS 545
#define OLD 546
#define ON 547
#define ONLY 548
#define OPERATOR 549
#define OPTION 550
#define OPTIONS 551
#define OR 552
#define ORDER 553
#define ORDINALITY 554
#define OTHERS 555
#define OUT_P 556
#define OUTER_P 557
#define OVER 558
#define OVERLAPS 559
#define OVERLAY 560
#define OVERRIDING 561
#define OWNED 562
#define OWNER 563
#define PACKAGE 564
#define PARALLEL 565
#define PARSER 566
#define PARTIAL 567
#define PARTITION 568
#define PASSING 569
#define PASSWORD 570
#define PERCENT 571
#define PERSISTENT 572
#define PIVOT 573
#define PIVOT_LONGER 574
#define PIVOT_WIDER 575
#define PLACING 576
#define PLANS 577
#define POLICY 578
#define POSITION 579
#define POSITIONAL 580
#define PRAGMA_P 581
#define PRECEDING 582
#define PRECISION 583
#define PREPARE 584
#define PREPARED 585
#define PRESERVE 586
#define PRIMARY 587
#define PRIOR 588
#define PRIVILEGES 589
#define PROCEDURAL 590
#define PROCEDURE 591
#define PROGRAM 592
#define PUBLICATION 593
#define QUALIFY 594
#define QUARTER_P 595
#define QUARTERS_P 596
#define QUOTE 597
#define RANGE 598
#define READ_P 599
#define REAL 600
#define REASSIGN 601
#define RECHECK 602
#define RECURSIVE 603
#define REF 604
#define REFERENCES 605
#define REFERENCING 606
#define REFRESH 607
#define REINDEX 608
#define RELATIVE_P 609
#define RELEASE 610
#define RENAME 611
#define REPEAT 612
#define REPEATABLE 613
#define REPLACE 614
#define REPLICA 615
#define RESET 616
#define RESPECT_P 617
#define RESTART 618
#define RESTRICT 619
#define RETURNING 620
#define RETURNS 621
#define REVOKE 622
#define RIGHT 623
#define ROLE 624
#define ROLLBACK 625
#define ROLLUP 626
#define ROW 627
#define ROWS 628
#define RULE 629
#define SAMPLE 630
#define SAVEPOINT 631
#define SCHEMA 632
#define SCHEMAS 633
#define SCOPE 634
#define SCROLL 635
#define SEARCH 636
#define SECOND_P 637
#define SECONDS_P 638
#define SECRET 639
#define SECURITY 640
#define SELECT 641
#define SEMI 642
#define SEQUENCE 643
#define SEQUENCES 644
#define SERIALIZABLE 645
#define SERVER 646
#define SESSION 647
#define SET 648
#define SETOF 649
#define SETS 650
#define SHARE 651
#define SHOW 652
#define SIMILAR 653
#define SIMPLE 654
#define SKIP 655
#define SMALLINT 656
#define SNAPSHOT 657
#define SOME 658
#define SQL_P 659
#define STABLE 660
#define STANDALONE_P 661
#define START 662
#define STATEMENT 663
#define STATISTICS 664
#define STDIN 665
#define STDOUT 666
#define STORAGE 667
#define STORED 668
#define STRICT_P 669
#define STRIP_P 670
#define STRUCT 671
#define SUBSCRIPTION 672
#define SUBSTRING 673
#define SUCH 674
#define SUMMARIZE 675
#define SYMMETRIC 676
#define SYSID 677
#define SYSTEM_P 678
#define TABLE 679
#define TABLES 680
#define TABLESAMPLE 681
#define TABLESPACE 682
#define TEMP 683
#define TEMPLATE 684
#define TEMPORARY 685
#define TEXT_P 686
#define THAT 687
#define THEN 688
#define TIES 689
#define TIME 690
#define TIMESTAMP 691
#define TO 692
#define TRAILING 693
#define TRANSACTION 694
#define TRANSFORM 695
#define TREAT 696
#define TRIGGER 697
#define TRIM 698
#define TRUE_P 699
#define TRUNCATE 700
#define TRUSTED 701
#define TRY_CAST 702
#define TYPE_P 703
#define TYPES_P 704
#define UNBOUNDED 705
#define UNCOMMITTED 706
#define UNENCRYPTED 707
#define UNION 708
#define UNIQUE 709
#define UNKNOWN 710
#define UNLISTEN 711
#define UNLOGGED 712
#define UNPIVOT 713
#define UNTIL 714
#define UPDATE 715
#define USE_P 716
#define USER 717
#define USING 718
#define VACUUM 719
#define VALID 720
#define VALIDATE 721
#define VALIDATOR 722
#define VALUE_P 723
#define VALUES 724
#define VARCHAR 725
#define VARIABLE_P 726
#define VARIADIC 727
#define VARYING 728
#define VERBOSE 729
#define VERSION_P 730
#define VIEW 731
#define VIEWS 732
#define VIRTUAL 733
#define VOLATILE 734
#define WEEK_P 735
#define WEEKS_P 736
#define WHEN 737
#define WHERE 738
#define WHITESPACE_P 739
#define WINDOW 740
#define WITH 741
#define WITHIN 742
#define WITHOUT 743
#define WORK 744
#define WRAPPER 745
#define WRITE_P 746
#define XML_P 747
#define XMLATTRIBUTES 748
#define XMLCONCAT 749
#define XMLELEMENT 750
#define XMLEXISTS 751
#define XMLFOREST 752
#define XMLNAMESPACES 753
#define XMLPARSE 754
#define XMLPI 755
#define XMLROOT 756
#define XMLSERIALIZE 757
#define XMLTABLE 758
#define YEAR_P 759
#define YEARS_P 760
#define YES_P 761
#define ZONE 762
#define NOT_LA 763
#define NULLS_LA 764
#define WITH_LA 765
#define POSTFIXOP 766
#define UMINUS 767




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 14 "third_party/libpg_query/grammar/grammar.y"
{
	core_YYSTYPE		core_yystype;
	/* these fields must match core_YYSTYPE: */
	int					ival;
	char				*str;
	const char			*keyword;
	const char          *conststr;

	char				chr;
	bool				boolean;
	PGJoinType			jtype;
	PGDropBehavior		dbehavior;
	PGOnCommitAction		oncommit;
	PGOnCreateConflict		oncreateconflict;
	PGList				*list;
	PGNode				*node;
	PGValue				*value;
	PGObjectType			objtype;
	PGTypeName			*typnam;
	PGObjectWithArgs		*objwithargs;
	PGDefElem				*defelt;
	PGSortBy				*sortby;
	PGWindowDef			*windef;
	PGJoinExpr			*jexpr;
	PGIndexElem			*ielem;
	PGAlias				*alias;
	PGRangeVar			*range;
	PGIntoClause			*into;
	PGCTEMaterialize			ctematerialize;
	PGWithClause			*with;
	PGInferClause			*infer;
	PGOnConflictClause	*onconflict;
	PGOnConflictActionAlias onconflictshorthand;
	PGAIndices			*aind;
	PGResTarget			*target;
	PGInsertStmt			*istmt;
	PGVariableSetStmt		*vsetstmt;
	PGOverridingKind       override;
	PGSortByDir            sortorder;
	PGSortByNulls          nullorder;
	PGIgnoreNulls          ignorenulls;
	PGConstrType           constr;
	PGLockClauseStrength lockstrength;
	PGLockWaitPolicy lockwaitpolicy;
	PGSubLinkType subquerytype;
	PGViewCheckOption viewcheckoption;
	PGInsertColumnOrder bynameorposition;
	PGLoadInstallType loadinstalltype;
	PGTransactionStmtType transactiontype;
}
/* Line 1489 of yacc.c.  */
#line 1124 "third_party/libpg_query/grammar/grammar_out.hpp"
	YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif



#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
} YYLTYPE;
# define yyltype YYLTYPE /* obsolescent; will be withdrawn */
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif


