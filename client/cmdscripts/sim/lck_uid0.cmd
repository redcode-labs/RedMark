rem "UFUID block0 freezer script started"
hf 14a raw -a -k -b 7 40
hf 14a raw    -k      43
hf 14a raw    -k -c   e000
hf 14a raw    -k -c   e100
hf 14a raw       -c   85000000000000000000000000000008
