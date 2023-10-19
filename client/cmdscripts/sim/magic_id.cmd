rem "Magic UID identificator script started"
hf 14a config --atqa force --bcc ignore --cl2 skip --rats skip
rem "Searching for 4b UID using ATQA (Magic TypeA):"
hf 14a reader
s -t 500
hf 14a config --atqa force --bcc ignore --cl2 force --cl3 skip --rats skip
rem "Searching for 7b UID using ATQA (Magic TypeB):"
hf 14a reader
s -t 500
hf 14a config --std
