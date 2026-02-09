from .. import USE_ACTUAL_SPARK

if USE_ACTUAL_SPARK:
    from pyspark.sql.column import *
else:
    from packdb.experimental.spark.sql.column import *
