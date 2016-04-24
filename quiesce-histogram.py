#! /usr/bin/env python
# usage: cat <result json file>| python quiesce-histogram.py

import json, sys
import collections

line = sys.stdin.readline()

decoded = json.JSONDecoder().decode(line)

history = {}

for t in decoded:
    h = t['history']
    for reclaim in h:
        start = reclaim['start']
        end = reclaim['end']
        history[start] = end - start

sortedHistory = collections.OrderedDict(sorted(history.items()))
firstTime = sortedHistory.items()[0][0]

for time in sortedHistory:
    print '%f %f' % (time - firstTime, history[time])
