REQUEST
<id> GET
<id> START
<id> STOP
<id> PERIOD <ms>
<id> THRESHOLD <mC>
<id> INJECT <mC>
<id> WATCH <0|1>

RESPONSE
RSP <id> OK ...
RSP <id> ERR ...

EVENT
EVT SAMPLE <seq> <timestamp_ns> <value_mC> <alarm>

UDP
STAT <udp_seq> <last_seq> <last_value_mC> <last_alarm> <dropped_total> <running>
