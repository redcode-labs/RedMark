lf conf --125
lf conf -s 500 -t 20 -d 3 -a
lf read -v 20000 -n
s -t 500
lf conf --134
lf conf -s 500 -t 20 -d 3 -a
lf read -v 20000 -n -@
lf conf -r

