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

# sequential test

