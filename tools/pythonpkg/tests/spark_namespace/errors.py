from . import USE_ACTUAL_SPARK

if USE_ACTUAL_SPARK:
    from pyspark.errors import *
else:
    from packdb.experimental.spark.errors import *
