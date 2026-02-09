from .. import USE_ACTUAL_SPARK

if USE_ACTUAL_SPARK:
    from pyspark.sql.functions import *
else:
    from packdb.experimental.spark.sql.functions import *
