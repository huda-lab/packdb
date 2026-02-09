from .. import USE_ACTUAL_SPARK

if USE_ACTUAL_SPARK:
    from pyspark.sql.catalog import *
else:
    from packdb.experimental.spark.sql.catalog import *
