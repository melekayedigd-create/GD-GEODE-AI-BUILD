# AI Level Builder (Geode mod)

Adds an **AI** button to the Geometry Dash level editor. Type a level idea, and an AI places the blocks, spikes, orbs, pads and portals for you.

## Get the .geode file (no installs needed)

1. Make a free account on **github.com**.
2. Click **+** (top right) → **New repository**. Name it `ai-level-builder`, set it to **Public**, and click **Create repository**.
3. On the new repo page, click **uploading an existing file**.
4. Drag in **everything inside this folder**: `mod.json`, `CMakeLists.txt`, `about.md`, `README.md`, the `src` folder and the `.github` folder.
   - The `.github` folder is hidden on some computers. On Windows: File Explorer → View → Show → **Hidden items**.
   - If dragging the `.github` folder doesn't work, create the file by hand: **Add file → Create new file**, type the name `.github/workflows/build.yml`, and paste in the contents of that file.
5. Click **Commit changes**.
6. Open the **Actions** tab. A build named *Build Geode Mod* starts on its own (takes about 5–10 minutes).
7. When it shows a green check, click it, scroll down to **Artifacts**, and download **Build Output**. Unzip it to get `myriam.ai-level-builder.geode`.

If the build shows a red X, open it, copy the error, and send it back to Claude.

## Put it on your Android phone

1. Send the `.geode` file to your phone (USB cable, Google Drive, Telegram to yourself, etc.).
2. Using a file manager on the phone, copy it to:
   `Android/media/com.geode.launcher/game/geode/mods/`
3. Fully close and reopen Geometry Dash through the Geode launcher.
4. The mod appears in Geode's **Installed** mods list.

## Set it up

1. Get a free API key at **aistudio.google.com** → **Get API key**.
2. In Geode, open **AI Level Builder** → settings (gear icon) → paste the key into **API Key**.
3. Open a level in the editor → tap **AI** → describe your level → **Build**.

**Undo AI** in the popup removes everything from the last build.

## Settings

| Setting | What it does |
|---|---|
| AI Provider | `gemini` (free), `claude`, `openai`, or `custom` |
| API Key | Your key for that provider |
| Model | Leave empty for the default model |
| Custom URL | For `custom`: any OpenAI-compatible chat URL (Groq, OpenRouter...) |
| Place Level At | After your existing objects, or at the level start |
| Max Objects | Safety limit per build (default 400) |
