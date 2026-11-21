#!/usr/bin/env python3
"""Boots nothing: drives an already-booted VM (out/qa/boot.sh) to open Browser at a URL
and take screenshots. Usage: run.py URL shot_prefix [wait_s] [scrolls]"""
import sys, time, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import qa
url, pre = sys.argv[1], sys.argv[2]
wait = float(sys.argv[3]) if len(sys.argv) > 3 else 12
scrolls = int(sys.argv[4]) if len(sys.argv) > 4 else 1
ser = os.path.join(os.path.dirname(qa.D), 'qa', 'serial.log')
for _ in range(60):                                   # the desktop is up
    if b'Launching GUI' in open(ser, 'rb').read(): break
    time.sleep(1)
time.sleep(4)
q = qa.mk()
def click(x, y):
    q.home(); time.sleep(.2); q.click(x, y)
click(891, 864); time.sleep(3)                        # Browser in the taskbar
click(1061, 96); time.sleep(1.5)                      # maximize
click(800, 153); time.sleep(.3)                       # address bar
for i in range(40): q.key('backspace', .02)
q.type(url); q.key('ret')
time.sleep(wait)
print(q.shot(pre + '0'))
for s in range(scrolls):
    q.home(); q.goto(800, 500)
    for i in range(8): q.cmd('mouse_move 0 0 -1', 0.08)
    time.sleep(1.5)
    print(q.shot(pre + str(s + 1)))
