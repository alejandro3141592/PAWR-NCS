# Node Manager — User Guide

Node Manager is the app you use to talk to the **central** hub device over
USB: scan for sensor nodes, start ("Init") them measuring, download their
stored data, and view it as a graph or export it to a spreadsheet file (CSV).

You do **not** need Python, or any of the source code, installed to use this
app — everything you need is in the folder you were given.

---

## 1. What you need

- A Windows PC.
- The **central** device (the hub board), connected to your PC with a USB
  cable.
- The `node_manager_gui` folder you were given. It must stay a whole folder —
  don't move or copy just the `.exe` file out on its own, it needs the other
  files sitting next to it to run.

---

## 2. Starting the app

1. Open the `node_manager_gui` folder.
2. Double-click `node_manager_gui.exe`.

The app window opens. If Windows shows a blue "Windows protected your PC"
warning the first time (this is normal for apps that aren't digitally
signed):

- Click **More info**
- Click **Run anyway**

If your antivirus flags it, that's also a known false-positive some
antivirus tools raise on Python apps packaged this way — allow it to run.

---

## 3. Connecting to the central device

1. Plug the central device into your PC with a USB cable, if you haven't
   already.
2. At the top of the window, use the **port dropdown** (next to the USB
   icon) to pick the right COM port.
   - If you only have one device plugged in, there's usually only one
     option to pick.
   - If nothing shows up, click the small **refresh (↻)** button next to
     the dropdown after plugging the device in.
   - If you have several similar boards plugged in at once and aren't sure
     which port is the central device, unplug everything else first, note
     which port appears, then plug the rest back in.
3. Click the **Connect** button (top right, turns to a red **Disconnect**
   button once connected).

The status text in the bottom-left of the window and the bottom status bar
will say **"Connected to COM\_\_"** once it works. If it instead shows
repeated "Opening COM\_\_... Access is denied" messages, something else on
your PC already has that port open — close any other program that might be
using it (another instance of this app, a serial monitor, etc.) and click
**Connect** again.

---

## 4. Finding nodes (Scan)

Once connected, click **Global Scan** (top right, next to Connect).

Nodes that are powered on and in Bluetooth range will start appearing as
rows in the table, each showing:

| Column | Meaning |
|---|---|
| **Node ID** | The node's number |
| **Signal** | Signal strength (closer to 0 is stronger, e.g. -50 is stronger than -80) |
| **State** | `UNKNOWN` = never started this session · `INITED` = started, currently measuring · `DOWNLOADED` = data has been pulled from it |
| **Status / Transfer** | What's happening right now (idle, connecting, downloading...) |
| **Last Sync** | When you last downloaded data from this node |
| **Data Entries** | How many readings are currently stored for this node in the app |

Click **Global Scan** again (it becomes **Stop Scan**) to stop scanning once
you've found what you need — you don't need to leave it scanning the whole
time.

A node only stays "available" while it's actively showing up in the scan.
If a node goes out of range or gets turned off, its **Init** and
**Download** buttons will grey out on their own after a few seconds, and
come back as soon as it's back in range and scanning again.

---

## 5. Starting a node (Init)

Before a node has ever measured anything, it needs to be told to start.
This is the **Init** button, in the Actions column of that node's row.

1. Make sure the node shows up in the table (see Scan, above).
2. Click **Init** on that node's row.
3. Wait for its **State** to change to `INITED`.

You only need to do this once per node per power cycle — if a node loses
power and comes back on, or you're not sure whether it's already been
started, it's safe to click Init again; it won't lose any data already
measuring/stored on the node.

---

## 6. Downloading data

Once a node has been running for a while (it stores a new reading roughly
every 10 seconds on its own, with no other action needed from you), you can
pull its stored data into this app.

1. Click **Download** on that node's row.
   - If the node's state is still `UNKNOWN` (never Init'd this session),
     you'll get a warning asking if you're sure — this usually means you
     might be about to pull old/leftover data rather than a fresh
     measurement run. Click **Yes** only if that's what you intend.
2. Watch the **Status / Transfer** column — it will show a live count of
   entries as they come in.
3. When it finishes, **State** becomes `DOWNLOADED`, **Last Sync** updates
   to the current time, and **Data Entries** shows the total count.

**You can download from the same node more than once** — each time, only
the *new* readings since your last download are pulled in, so repeat
downloads are fast and never re-fetch data you already have.

---

## 7. Viewing the graph

Click **Graph** on a node's row (only available once that node has data).

A window opens showing temperature and humidity over time for that node.

- **Show real time (approximate)** checkbox: toggles between showing actual
  clock time, or time-since-that-node-started. Real time is only as
  accurate as how soon after starting the node you first downloaded its
  data — if in doubt, use the time-since-start view, which is always
  exact.
- **From / To sliders**: drag to zoom into a specific portion of the data.
  Click **Reset Range** to go back to showing everything.
- **Export PNG**: saves the graph currently on screen as an image file, so
  you can put it in a report or send it to someone.

---

## 8. Exporting data to a spreadsheet (CSV)

Click **Export** on a node's row (only available once that node has data).

A folder picker opens — choose where to save. The app writes one file named
like `node37_20260816_143000.csv` (node number + date + time) into that
folder. This file opens directly in Excel or any spreadsheet program, with
one row per reading: sequence number, time since start (milliseconds),
temperature (°C), and humidity (%).

---

## 9. The terminal panel at the bottom

The **Serial Console Output** panel at the bottom of the window shows the
raw messages going back and forth with the central device. You don't need
to read this for normal use — it's there for troubleshooting if something
isn't behaving as expected. Click **Clear** to empty it.

---

## Troubleshooting

**The port dropdown is empty / my device isn't listed.**
Unplug and replug the USB cable, then click the refresh (↻) button next to
the dropdown. Make sure the device is actually the central hub, not one of
the wearable sensor nodes (nodes don't connect over USB to this app at
all — only the central hub does).

**"Access is denied" when trying to connect.**
Another program already has that COM port open. Close other serial-monitor
tools, other copies of this app, or anything else that might be using it,
then try **Connect** again.

**A node's Init/Download buttons are greyed out.**
That node hasn't shown up in a scan recently. Click **Global Scan** and
wait for it to appear (make sure it's powered on and in range).

**Download seems stuck / not moving.**
Bluetooth range/interference can occasionally stall a transfer. Wait a
minute; if it doesn't recover, click **Download** again — it will safely
pick up from where it left off rather than starting over.

**A node's graph shows a wall-clock time that seems wrong (off by minutes
or more).**
This happens when a node has been running for a while before its first-ever
download. Switch off **"Show real time"** to see time-since-start instead,
which is always accurate. To get accurate real time going forward, restart
that specific node fresh (power cycle it, then Init it and download shortly
after).

**Windows or antivirus won't let the app run.**
See the note under [Starting the app](#2-starting-the-app) above — this is
a common false-positive for unsigned Python apps, not a real problem with
the app itself.
