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

# for a game with no route of its own: through whatever its title screens and menus take
GENERIC = ['WAIT 600'] + ['A 6', 'WAIT 90', 'T@160/120 4', 'WAIT 90', 'START 6', 'WAIT 90'] * 120

# Kid Icarus: Uprising. Through the title, the file and the story as GENERIC goes; from there on START would pause the game every few
# seconds (the user, 2026-11-11), so: A to leave a pause, then Pit flies, shoots and steers through chapter 1's air battle (five minutes on
# rails) and on into its land battle, walking and shooting, with a drag to turn him now and then and an A for whatever asks for one.
KID_ICARUS_PLAY = ['CUP+L+T@160/110 150', 'CLEFT+L+T@130/120 90', 'CUP+L 150', 'DRAG 110/120 200/120 12', 'CRIGHT+L+T@190/120 90', 'CUP+L 150',
                   'DRAG 210/120 120/120 12', 'A 6']

# New Super Mario Bros. 2: to the world map on A (it's there within a minute), right to World 1-1's place on it, and in
NSMB2_INTO_1_1 = A(55) + ['CRIGHT 20', 'WAIT 90', 'A 6', 'WAIT 360']

# Pokemon Ultra Sun and Ultra Moon from a save kept on Route 1, the first partner just chosen (sources.json's "save"): through the title
# into the game, left into the tall grass and round in it. A wild Pokemon comes within seconds, and the battle is fought by touch, which
# does nothing while walking: Fight, then the second move (Leafage). B closes what an A too many opened, and runs; in a battle the stick
# only moves the cursor about, and nothing presses A on it.
USUM_BATTLE = ['WAIT 900'] + A(45, 90) + ['B 6', 'WAIT 60'] * 6 + ['CLEFT+B 240'] + [
    line for d in ('CLEFT', 'CUP', 'CRIGHT', 'CDOWN') * 100 for line in (f'{d}+B 90', 'T@280/120 10', 'WAIT 50', 'T@250/85 10', 'WAIT 50')]

# Mario Kart 7: A through the title and every menu's first choice (Single Player, Grand Prix, 50cc, Mario, his kart, the Mushroom Cup)
# into the first race, then A held: the kart drives on, into walls and round them, among the other racers.
MK7_RACE = ['WAIT 600'] + A(30, 120) + ['A 600', 'WAIT 10'] * 40

ROUTES = {
    'mario_kart_7': MK7_RACE,
    # the same, held on through the cup: each race ends four to five minutes in with the kart against walls, and the third is Cheep Cheep
    # Lagoon, whose water is where a console loses the most frames (the user, 2026-11-14). A course can't be swapped in for the first: the game
    # never finishes loading another course's file under Toad Circuit's name.
    'mario_kart_7_cup': MK7_RACE + ['A 600', 'WAIT 10'] * 60,
    # the Shell Cup, the other cup a blank save has open (retro courses): down before A from the cc choice on, so the cup choice takes the
    # second row; elsewhere it only picks the next cc, racer or part
    'mario_kart_7_shell': ['WAIT 600'] + A(3, 120) + ['CDOWN 6', 'WAIT 30', 'A 6', 'WAIT 120'] * 8 + A(19, 120) + ['A 600', 'WAIT 10'] * 100,
    'usum_battle': USUM_BATTLE,
    # from the same save, into the game and no further: the place the save was made at, drawn frame after frame, the same in every run
    'usum_stand': ['WAIT 900'] + A(4, 90) + ['B 6', 'WAIT 60'] * 4 + ['WAIT 30000'],
    'kid_icarus': GENERIC[:1 + 6 * 7] + A(2, 120) + KID_ICARUS_PLAY * 80,
    # Mario stays where the level starts: he jumps on the spot, the game is paused for 40 s, he jumps again. The check's pictures, one
    # every 33 s, then show the same screens every run. Running right he died somewhere else each time, and runs with nothing wrong were
    # flagged for their pictures (Continue in one, Game Over in another).
    'nsmb2': NSMB2_INTO_1_1 + ['WAIT 300'] + A(12, 100) + ['WAIT 400', 'START 6', 'WAIT 2400', 'START 6', 'WAIT 120'] + A(6, 100) + ['WAIT 30000'],
    # running right with a jump now and then, to wherever that gets him: for what a level draws further in
    'nsmb2_run': NSMB2_INTO_1_1 + run_right(150),
    # in World 1-1 the options menu opens (a route's KEY menu, as a click of the left stick) and stays up, on the TV and the GamePad
    'nsmb2_menu': NSMB2_INTO_1_1 + ['WAIT 300', 'KEY menu', 'WAIT 30000'],
    # Animal Crossing from a kept town save: the title's Continue, the first player, into town, then the Touch Screen's menus
    'acnl_design': ['WAIT 900'] + A(100, 60) + ['WAIT 600', 'X 6', 'WAIT 300', 'T@78/11 10', 'WAIT 300', 'T@35/82 10', 'WAIT 300', 'T@50/182 10', 'WAIT 400', 'A 6', 'WAIT 400', 'T@35/82 10', 'WAIT 400', 'T@50/182 10', 'WAIT 600', 'DRAG 120/70 200/70 40', 'WAIT 120', 'DRAG 120/100 200/160 40', 'WAIT 120', 'DRAG 200/100 120/160 40', 'WAIT 120', 'T@160/120 10', 'WAIT 600', 'WAIT 30000'],
    # standing where the level starts for the whole run: one place, drawn frame after frame, for timing a level against another
    'nsmb2_stand': NSMB2_INTO_1_1 + ['WAIT 30000'],
    'dream_team': A(8) + ['DDOWN 6', 'WAIT 40', 'A 6', 'WAIT 80'] * 6 + A(440, 30) + sweep(WALK) + A(500, 30),
    'sticker_star': A(6) + START(4) + A(300, 40) + sweep(WALK) + A(300, 40),
    'paper_jam': A(8) + A(500, 40) + sweep(WALK) + A(300, 40),
    'superstar_saga': A(12) + A(500, 40) + sweep(WALK) + A(300, 40),
    'bowsers_inside': A(8) + T(160, 116, 240) + T(160, 40, 240) + T(160, 60, 240) + T(160, 60, 240) + A(500, 40) + sweep(WALK) + A(300, 40),
    'layton_mask': ['WAIT 1300', 'T@170/119 3', 'WAIT 500'] + (taps() + A(10, 30)) * 30,
    'layton_azran': ['WAIT 1300', 'T@170/119 3', 'WAIT 500'] + (taps() + A(10, 30)) * 30,
    'tomodachi': A(20, 90) + (taps() + A(10, 30)) * 30,
}
