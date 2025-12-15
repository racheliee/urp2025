make clean
make all

gcc -O3 compare.c -o compare

ssh eternity2 "pkill server"
#ssh eternity2 "rm -f /urp2025/*"
scp server eternity2:/urp2025/jhtest
#scp copy/server_valid eternity2:/urp2025
#ssh eternity2 "sudo /urp2025/server" &
#ssh -t eternity2 'cd /urp2025 && exec ./server'
