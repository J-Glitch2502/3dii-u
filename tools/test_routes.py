"""Scripted play from a blank save, one list of input lines per game (services/hid.cpp reads them). The Cemu runs write them to live.txt, console_tour.py sends them over the network."""

A = lambda n, gap=60: ['A 6', f'WAIT {gap}'] * n
T = lambda x, y, wait=90: [f'T@{x}/{y} 10', f'WAIT {wait}']
START = lambda n, gap=120: ['START 6', f'WAIT {gap}'] * n


def taps(ys=(160, 178, 195), xs=range(20, 291, 45), gap=30):
    return [line for y in ys for x in xs for line in (f'T@{x}/{y} 3', f'WAIT {gap}')]


def sweep(moves, gap=40):
    # walking around, pressing A on whatever comes up
    return [line for d, n in moves for _ in range(n) for line in (f'{d} 60', 'WAIT 5', 'A 6', f'WAIT {gap}')]


def run_right(n):
    return ['CRIGHT 50', 'CRIGHT+A 14', 'CRIGHT 40', 'CRIGHT+A 20'] * n


WALK = (('CDOWN', 12), ('CRIGHT', 14), ('CLEFT', 28), ('CUP', 8), ('CRIGHT', 14))

ROUTES = {
    'nsmb2': A(170) + run_right(120),
    'dream_team': A(8) + ['DDOWN 6', 'WAIT 40', 'A 6', 'WAIT 80'] * 6 + A(440, 30) + sweep(WALK) + A(500, 30),
    'sticker_star': A(6) + START(4) + A(300, 40) + sweep(WALK) + A(300, 40),
    'paper_jam': A(8) + A(500, 40) + sweep(WALK) + A(300, 40),
    'superstar_saga': A(12) + A(500, 40) + sweep(WALK) + A(300, 40),
    'bowsers_inside': A(8) + T(160, 116, 240) + T(160, 40, 240) + T(160, 60, 240) + T(160, 60, 240) + A(500, 40) + sweep(WALK) + A(300, 40),
    'layton_mask': ['WAIT 1300', 'T@170/119 3', 'WAIT 500'] + (taps() + A(10, 30)) * 30,
    'layton_azran': ['WAIT 1300', 'T@170/119 3', 'WAIT 500'] + (taps() + A(10, 30)) * 30,
    'tomodachi': A(20, 90) + (taps() + A(10, 30)) * 30,
}
