rem "MFU revival script started"
hf 14a config --atqa force --bcc ignore --cl2 force --cl3 skip --rats skip
hf mfu setuid --uid 04112233445566
hf 14a config --std
hf 14a reader
