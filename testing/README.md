# Expected Directory Hierarchy
```
urp2025(..)
    └testing(.)
        └compare.c
        └finegrained_read_test.sh
        └finegrained_write_test.sh
        └random_test.sh
        └sequential_test.sh
        └outputs(random_logs/ finegrained_read_logs/ finegrained_write_logs/ sequential_logs/)
            └YYYYMMDD-HHmmSS
    └sequential_block_read
        └create_file
        └client
        └baseline
    └finegrained_block_read
        └create_file
        └client_read
        └client_write
        └baseline_read
        └baseline_write
    └random_block_read
        └create_file
        └client_random
        └baseline_random
```

# Sequential Test
1. env check (necessary commands and binaries)
2. create log directory named with current timestamp
3. create test file + copy
4. drop cache
for 30:
5. run client, baseline
6. (Optional, line 103 - 106) compare client and baseline for sanity check

# Finegrained Read Test
1. env check (necessary commands and binaries)
2. create log directory named with current timestamp
3. create test file + copy
4. drop cache
for 30: 
5. run client, baseline

# Finegrained Write Test
1. env check (necessary commands and binaries)
2. create log directory named with current timestamp
3. create test file + copy
4. drop cache
for 30:
5. run client, baseline
6. (Optional, line 98 - 101) compare client and baseline for sanity check

# Random Test
1. env check (necessary commands and binaries)
2. create log directory named with current timestamp
3. create test file + copy
4. drop cache
for 30:
5. (Optional, line 215 - 235) compare client, baseline for sanity check
6. run baseline
7. run client
8. flush cache

# How to Run
```
sudo ./<test_name>.sh
```
