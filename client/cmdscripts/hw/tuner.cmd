rem "LF tune with 134 kHz divisor"
lf tune --mix -t 5 -q 88
rem "LF tune with 1125kHz divisor"
lf tune --mix -t 5 -q 95
rem "HF tune"
hf tune --iter 1000 --mix