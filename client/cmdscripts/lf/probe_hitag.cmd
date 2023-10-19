lf cmdread -d 50 -z 116 -o 166 -e W3000 -c W00110
s -t 1000
lf cmdread -d 50 -z 116 -o 166 -e W3000 -c W11000
s -t 1000
lf cmdread -d 50 -z 116 -o 166 -e W3000 -c W11000 -s 2000 -@
s -t 1000
lf cmdread -d 48 -z 112 -o 176 -e W3000 -e S240 -e E336 -c W0S00000010000E
lf read -n -@ -s 15000
