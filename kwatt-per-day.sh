#!/bin/bash
awk -F, -f kwatt-per-day.awk < /var/log/currentcost/currentcost.csv | sort -n
