@echo off
rem The builder, making games that measure themselves (timers, the [prof] lines, the network console on port 4405) and packing them for a
rem console all the same: for profiling a game on a Wii U with tools/console_watch.py. Slower than a normal build; not for playing.
set THREEDII_MEASURE=console
start "" pythonw "%~dp0..\..\builder\3Dii U Builder.pyw"
