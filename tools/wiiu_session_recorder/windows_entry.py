"""Windows executable entry point, including a packaged GUI/transport smoke check."""
from pathlib import Path
import sys
import traceback

def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--smoke-test':
        from windows_smoke import run
        run(Path(sys.argv[2]))
        return 0
    from recorder import main as record
    return record()

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception:
        destination = Path.home() / 'Wii U' / 'Logs' / 'recorder-startup-error.txt'
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(traceback.format_exc())
        import tkinter.messagebox
        tkinter.messagebox.showerror('Wii U Session Recorder', 'The recorder could not start. Details: ' + str(destination))
        raise
