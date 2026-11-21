"""Am-Droid's voice-command toolset and its frozen suite, written to Cactus's rules for Needle 3:
one tool per action, names people say, closed sets as enums, bounded numbers, free text copied
word for word, refusal ([]) for anything missing, irrelevant, negated or out of bounds.
    python3 os_suite.py os_suite.json     (writes the suite; the OS embeds TOOLS/SYSTEM as written here)
"""
import json
import sys

APPS = ["files", "notes", "browser", "calculator", "clock", "settings", "tasks", "app store", "network",
        "video player", "about", "terminal"]

TOOLS = [
    {"name": "open_app", "description": "Open an app on the desktop.",
     "parameters": {"type": "object", "properties": {
         "app": {"type": "string", "enum": APPS, "description": "The app to open."}}, "required": ["app"]}},
    {"name": "close_app", "description": "Close an app's window. With no app named, close the window in front.",
     "parameters": {"type": "object", "properties": {
         "app": {"type": "string", "enum": APPS, "description": "The app to close; include only when the user names one."}}}},
    {"name": "search_web", "description": "Search the web or open a website in the browser.",
     "parameters": {"type": "object", "properties": {
         "query": {"type": "string", "description": "The search words or web address, copied word for word.",
                   "minLength": 1, "maxLength": 100}}, "required": ["query"]}},
    {"name": "open_website", "description": "Open a website in the browser by its address, like example.com.",
     "parameters": {"type": "object", "properties": {
         "address": {"type": "string", "description": "The web address, copied word for word.", "minLength": 3, "maxLength": 100}},
         "required": ["address"]}},
    {"name": "set_volume", "description": "Set the sound volume.",
     "parameters": {"type": "object", "properties": {
         "level": {"type": "integer", "description": "Volume from 0 to 100.", "minimum": 0, "maximum": 100}},
         "required": ["level"]}},
    {"name": "set_brightness", "description": "Set the screen brightness.",
     "parameters": {"type": "object", "properties": {
         "level": {"type": "integer", "description": "Brightness from 0 to 100.", "minimum": 0, "maximum": 100}},
         "required": ["level"]}},
    {"name": "do_not_disturb", "description": "Turn Do Not Disturb on or off. On silences notifications.",
     "parameters": {"type": "object", "properties": {
         "action": {"type": "string", "enum": ["on", "off"], "description": "on or off."}}, "required": ["action"]}},
    {"name": "add_note", "description": "Write a note in the Notes app.",
     "parameters": {"type": "object", "properties": {
         "text": {"type": "string", "description": "The note, copied word for word.", "minLength": 1, "maxLength": 200}},
         "required": ["text"]}},
    {"name": "show_date_and_time", "description": "Show today's date and the current time. Takes no arguments.",
     "parameters": {"type": "object", "properties": {}}},
]

SYSTEM = ("Map each explicit supported desktop action to exactly one declared call. Do not guess missing values. "
          "Unsupported, invalid, ambiguous, and negated requests return no call.")


def c(name, **args):
    return {"name": name, "arguments": args}


CASES = [
    # positive
    ("open notes", [c("open_app", app="notes")], "positive"),
    ("open the browser", [c("open_app", app="browser")], "positive"),
    ("launch the calculator", [c("open_app", app="calculator")], "positive"),
    ("show me my files", [c("open_app", app="files")], "positive"),
    ("open settings", [c("open_app", app="settings")], "positive"),
    ("start the video player", [c("open_app", app="video player")], "positive"),
    ("open the app store", [c("open_app", app="app store")], "positive"),
    ("open the clock", [c("open_app", app="clock")], "positive"),
    ("open the terminal", [c("open_app", app="terminal")], "positive"),
    ("show the task manager", [c("open_app", app="tasks")], "positive"),
    ("close this window", [c("close_app")], "positive"),
    ("close the browser", [c("close_app", app="browser")], "positive"),
    ("close notes", [c("close_app", app="notes")], "positive"),
    ("search for chocolate cake recipes", [c("search_web", query="chocolate cake recipes")], "positive"),
    ("look up the weather in Lagos", [c("search_web", query="the weather in Lagos")], "positive"),
    ("go to example.com", [c("open_website", address="example.com")], "positive"),
    ("search the web for how to build an operating system", [c("search_web", query="how to build an operating system")], "positive"),
    ("set the volume to 40", [c("set_volume", level=40)], "positive"),
    ("turn the volume down to 10", [c("set_volume", level=10)], "positive"),
    ("mute the sound", [c("set_volume", level=0)], "positive"),
    ("set the brightness to 70", [c("set_brightness", level=70)], "positive"),
    ("dim the screen to 30", [c("set_brightness", level=30)], "positive"),
    ("turn on do not disturb", [c("do_not_disturb", action="on")], "positive"),
    ("turn off do not disturb", [c("do_not_disturb", action="off")], "positive"),
    ("silence my notifications", [c("do_not_disturb", action="on")], "positive"),
    ("take a note buy milk and eggs", [c("add_note", text="buy milk and eggs")], "positive"),
    ("write down call the dentist tomorrow", [c("add_note", text="call the dentist tomorrow")], "positive"),
    ("what time is it", [c("show_date_and_time")], "positive"),
    ("what's the date today", [c("show_date_and_time")], "positive"),
    # parallel
    ("open notes and set the volume to 25", [c("open_app", app="notes"), c("set_volume", level=25)], "parallel"),
    ("close the browser and open the calculator", [c("close_app", app="browser"), c("open_app", app="calculator")], "parallel"),
    # missing
    ("turn the volume up a bit", [], "missing"),
    ("make the screen brighter", [], "missing"),
    ("open an app", [], "missing"),
    ("search for something", [], "missing"),
    ("take a note", [], "missing"),
    # irrelevant
    ("send an email to my boss", [], "irrelevant"),
    ("order a pizza", [], "irrelevant"),
    ("shut down the computer", [], "irrelevant"),
    ("play some jazz", [], "irrelevant"),
    ("tell me a joke", [], "irrelevant"),
    ("set an alarm for 7 am", [], "irrelevant"),
    # negation
    ("don't open the browser", [], "negation"),
    ("do not close notes", [], "negation"),
    ("never turn the volume up to 100", [], "negation"),
    # invalid
    ("set the volume to 150", [], "invalid"),
    ("set the brightness to 300", [], "invalid"),
    ("open photoshop", [], "invalid"),
    # transcripts as Whisper writes them (capitals, punctuation)
    ("Open Notes.", [c("open_app", app="notes")], "whisper"),
    ("Set the volume to 40.", [c("set_volume", level=40)], "whisper"),
]

if __name__ == "__main__":
    suite = {"am_droid": {"tools": TOOLS, "system": SYSTEM,
                          "cases": [{"query": q, "calls": calls, "category": cat} for q, calls, cat in CASES]}}
    json.dump(suite, open(sys.argv[1], "w"), indent=1)
    print(len(CASES), "cases;", len(json.dumps(TOOLS, separators=(",", ":"))), "bytes of tools")
