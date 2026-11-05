# OrreLink Hub

A tiny web page plus Cloudflare Worker for OrreLink (Pokémon XD GBA netplay):

- **Team sharing.** Drop a Gen 3 save (Emerald, FireRed, LeafGreen, Ruby, Sapphire) on the page. The Pokémon and trainer name are read in the browser into a 617-byte team code, which the Worker stores for 30 days under an 8-character code. The save file itself never leaves the browser.
- **Open rooms.** Lists OrreLink rooms published to Dolphin's public session index, with their battle format.
- **Buttons that open the app.** `orrelink://search`, `orrelink://host?format=Orre`, `orrelink://join?method=traversal&code=…&port=…` and `orrelink://team/CODE`. The OrreLink build with the `orrelink://` handler is what makes these work; without it, use the app's own Host / Join / Search for Match buttons (they do the same thing).

There is deliberately **no matchmaking server**: "random opponent" is OrreLink's existing Search for Match.

## Run it locally (no account, no install)

    node test/worker.test.mjs      # unit tests
    node test/devserver.mjs        # http://localhost:8787 (in-memory team store)

## Deploy (free Cloudflare account)

    cd orrelink-hub
    npm install
    npx wrangler login
    npx wrangler kv namespace create TEAMS     # paste the printed id into wrangler.toml
    npx wrangler deploy                        # prints https://orrelink-hub.<you>.workers.dev
