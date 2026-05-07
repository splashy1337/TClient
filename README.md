# Moderator Client

A custom [DDraceNetwork](https://ddnet.org) client built for KoG moderators. Forked from [TClient](https://github.com/sjrc6/TaterClient-ddnet), which is itself a fork of [DDNet](https://github.com/ddnet/ddnet).

Includes all standard TClient and DDNet features, plus a moderator overlay panel, Discord bridge integration, and chat watcher.

---

## Building from source

> If you received a pre-built `DDNet.exe`, skip this section.

**Prerequisites:** Visual Studio 2022, CMake 3.20+, Git

```bat
git clone https://github.com/splashy1337/TClient.git
cd TClient
git submodule update --init --depth 1
cmake -B build
cmake --build build --config Release
```

The executable will be at `build\release\DDNet.exe`.

---

## Setup guide

### 1. First launch

Run `DDNet.exe`. On the first launch it will create your settings folder at:

```
%APPDATA%\DDNet\
```

Your config, screenshots, and demos are stored here. The moderator panel settings are saved in this folder as well.

---

### 2. RCON credentials

** USE AT OWN RISK **
(Both the RCON login and normal server login checks if it's actually a KoG server and does not execute on other servers, but better be safe then sorry)

The moderator panel auto-connects to RCON when you join a KoG server if credentials are configured.

Go to **Settings → Moderation** and fill in:

| Field | What to enter |
|-------|--------------|
| RCON Username | Your RCON username (leave blank if the server uses password-only auth) |
| RCON Password | Your RCON password |
| KoG Login Code | Your `/login` code for KoG servers (optional) |

These are saved encrypted in your local DDNet config. They are never transmitted anywhere except directly to the game server when you connect.

---

### 3. Moderator panel

Press the panel toggle keybind (default: bind one yourself via the console) or run:

```
mc_toggle_moderator_panel
```

The panel shows:
- **Players tab** — live player list from RCON `status`, with ban-line copy, attachment download, and mark-done buttons
- **Chat log tab** — flagged chat messages from the chat watcher

**Useful console commands:**

```
mc_toggle_moderator_panel       — open/close the panel
mc_add_todo <name>              — mark a player name red on the scoreboard
mc_clear_todos                  — clear the to-do list
mc_watcher_add <word>           — add a word to the chat watcher list
mc_watcher_remove <word>        — remove a word
mc_watcher_clear_log            — clear the flagged message log
mc_watcher_reload               — reload the wordlist file from disk
```

---

### 4. Discord bridge (optional)

The Discord bridge is a Python script that polls a KoG Discord forum channel and writes player names to a local JSON file that the moderator panel reads. This populates the to-do list automatically from Discord reports.

> [!IMPORTANT]
> The bridge accesses a private KoG moderator channel directly using your Discord account. **Your account must already be a KoG moderator with access to that channel** — if it doesn't, the bridge will fail to fetch anything. There is no workaround for this; it is a permission check on Discord's side.

#### Step 1 — Install Python

Download and install **Python 3.11+** from [python.org](https://www.python.org/downloads/).  
Make sure **"Add Python to PATH"** is checked during installation.

#### Step 2 — Install dependencies

Open a terminal in the `moderatorclient_scripts\` folder and run:

```bat
pip install -r requirements.txt
```

#### Step 3 — Discord token

> [!WARNING]
> Using a self-bot (user token automation) violates Discord's Terms of Service. The bridge runs passively and only reads a single channel, so the risk is very low in practice — but a ban is possible. Use at your own discretion.

The bridge needs a Discord **user token** (not a bot token) to read the forum channel.

Create the file:

```
%APPDATA%\DDNet\discord_token.txt
```

Paste your Discord user token as a single line with no extra spaces. The bridge reads it from there so the token is never stored in the script or config.

Alternatively, set the environment variable `MODERATORCLIENT_DISCORD_TOKEN` before launching.

> **How to get your Discord token:** Open Discord in a browser, open DevTools (F12), go to the Network tab, send any message, find a request to `discord.com/api`, and look for the `Authorization` header. This token gives full access to your account — keep it private.

#### Step 4 — Configure the script path

In **Settings → Moderation**, set the **Discord bridge script** field to the full path of `discord_bridge.py`, for example:

```
C:\path\to\TClient\moderatorclient_scripts\discord_bridge.py
```

If the field is left empty, the client will try to auto-detect the script relative to `DDNet.exe`.

#### Step 5 — Start the bridge

Click **Start bridge** in the moderator panel, or enable **Auto-start on launch** in Settings → Moderation to have it start automatically every time you open the client.

The bridge runs silently in the background (`pythonw.exe`). The moderator panel polls its output every 5 seconds and updates the player list.

---

### 5. Chat watcher

The chat watcher monitors in-game chat for configured words and logs flagged messages to the panel's Chat log tab. It only activates on KoG servers.

The wordlist is stored at:

```
%APPDATA%\DDNet\watcher_words.txt
```

One word or phrase per line. Lines starting with `#` are comments. Edit the file directly or use `mc_watcher_add` / `mc_watcher_remove` from the console.

---

### 6. Attachment downloads

When a player in the to-do list has Discord report attachments (images, screenshots), the moderator panel can download them directly. Click the **DL** button next to a player name in the panel.

Files are saved to:

```
%APPDATA%\DDNet\attachments\<player name>\<filename>
```

You can change the save directory in **Settings → Moderation → Attachment directory**.

---

## Config reference

All moderator settings are prefixed with `mc_` in the DDNet console:

| Config key | Default | Description |
|------------|---------|-------------|
| `mc_rcon_username` | *(empty)* | RCON username |
| `mc_rcon_password` | *(empty)* | RCON password |
| `mc_kog_login_code` | *(empty)* | KoG `/login` code |
| `mc_attachment_dir` | *(empty)* | Attachment save directory (empty = DDNet config dir) |
| `mc_moderator_panel_open` | `0` | Whether the panel is open |
| `mc_auto_start_discord_bridge` | `0` | Auto-start bridge on launch |
| `mc_discord_bridge_script` | *(empty)* | Full path to `discord_bridge.py` |
| `mc_discord_channel_id` | *(empty)* | Discord forum channel ID to poll (empty = built-in default) |
| `mc_chat_watcher` | `0` | Enable/disable the chat watcher |

---

## Attribution

Moderator Client is built on top of:
- [DDNet](https://github.com/ddnet/ddnet) — the base game client
- [TClient](https://github.com/sjrc6/TaterClient-ddnet) — extended client features

Thanks to the DDNet and TClient developers for their work.
