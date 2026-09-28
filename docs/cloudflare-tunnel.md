# Cloudflare Tunnel: publishing the render server

`inkboard.signalwave.dev` is served by a Cloudflare Tunnel from whichever machine runs the
`server/` Compose stack. The setup matches `~/finance-ai` and `~/chug-a-lug`: a `cloudflared`
container in the same Compose project holds the tunnel token, and the tunnel's public hostname
points at the Docker service name `http://inkboard:8000`. No port is opened on the router.

```
board ──HTTPS──▶ Cloudflare edge ──tunnel──▶ cloudflared (container) ──▶ http://inkboard:8000
browser ─┘        (TLS, cache bypass)          same Compose network        render server
```

Unlike finance-ai, **don't put Cloudflare Access in front of this hostname.** Boards can't
complete a login, and the service is meant to be public. Abuse is limited by the server's
per-IP rate limit (spec §3.5) and, optionally, a Cloudflare rate-limiting rule (step 5).

Menu names follow Cloudflare's dashboard as of September 2026. It gets reorganized from time
to time, so if a menu has moved, search the docs for the page name.

## One-time Cloudflare setup

1. **Create the tunnel.**
   - Go to Zero Trust → Networks → Tunnels → Create a tunnel → Cloudflared, and name it
     `inkboard`.
   - Make it a new tunnel. Don't reuse finance-ai's or chugalug's: one tunnel per Compose
     project keeps their connectors independent.
   - Copy the token (the string after `--token` in the install command) into `server/.env`
     as `CLOUDFLARE_TUNNEL_TOKEN`.
   - Skip installing a connector on the host. The `cloudflared` container is the connector.
2. **Add the public hostname.**
   - Go to Tunnels → `inkboard` → Public hostnames (or Routes → Add route → Published
     application).
   - Subdomain `inkboard`, domain `signalwave.dev`, service type `HTTP`, URL `inkboard:8000`.
   - Cloudflare creates the proxied DNS record for you.
3. **Bypass the cache for this hostname.**
   - Go to signalwave.dev → Caching → Cache Rules → Create rule, and name it "inkboard: no
     cache".
   - Match when Hostname equals `inkboard.signalwave.dev`, with the action **Bypass cache**.
   - Why: `.bin` and `.png` are on Cloudflare's list of cached extensions. The server already
     sends `Cache-Control: no-cache`, but the rule makes sure the edge never serves one
     board's frame, or an old one, from its cache. The ETag/304 exchange happens between the
     board and the server.
4. **Don't challenge the boards.**
   - Under Security → Bots, leave **Bot Fight Mode** off for the zone. Don't turn on
     "Under Attack" mode for this hostname either.
   - Why: an ESP32 can't solve a JavaScript challenge. It would get HTML instead of a frame,
     count that as a failure, and eventually show the offline badge.
   - If Bot Fight Mode must stay on for other sites in the zone, add a WAF custom rule
     instead: Hostname equals `inkboard.signalwave.dev`, action **Skip**, all remaining
     custom rules and Super Bot Fight Mode.
5. **(Optional) Rate-limit at the edge.**
   - Go to Security → WAF → Rate limiting rules → Create rule.
   - Match when Hostname equals `inkboard.signalwave.dev` and URI Path starts with
     `/v1/frame`, counted per IP.
   - Limit: 60 requests per 10 minutes, action Block for 10 minutes. The free plan allows
     one rule with a 10-second period; use 20 requests per 10 seconds if that is all that's
     available.
   - This sits in front of the server's own 30-per-hour limit. It sheds floods before they
     reach your machine; the server's limit is the one boards normally meet.

## On the server machine

```bash
cd server
cp .env.example .env        # fill in FRED_API_KEY and CLOUDFLARE_TUNNEL_TOKEN
docker compose up -d --build
docker compose logs -f cloudflared    # wait for "Registered tunnel connection" (usually 4 lines)
```

- `restart: unless-stopped` brings both containers back after a reboot. The site is public
  only while the stack is up: `docker compose stop` takes it offline.
- **Don't also install `cloudflared` as a host service** with this token. Two connectors
  split the traffic, and the host one can't resolve `inkboard:8000`. If one exists, remove
  it with `sudo cloudflared service uninstall`.
- The container publishes `127.0.0.1:18440` for local checks only. Nothing listens on a
  public interface.

## Client IPs and the rate limit

- Behind the tunnel, every request reaches the server from the `cloudflared` container, so
  the socket address is useless for per-IP limits.
- The server reads the client address from the header named in `INKBOARD_CLIENT_IP_HEADER`,
  which is set to `CF-Connecting-IP` in `compose.yaml`. Cloudflare always overwrites that
  header with the real client IP, so a client can't spoof it through the tunnel.
- Don't use `X-Forwarded-For` for this. Clients can pre-fill it, and Cloudflare appends to
  it rather than replacing it.
- The header can be trusted only because the server is reachable solely through the tunnel
  and from localhost. Keep the `127.0.0.1:` prefix on the published port.

## TLS as seen by the board

- The board talks HTTPS to Cloudflare's edge. The certificate is Cloudflare's Universal SSL
  certificate for `signalwave.dev`.
- Cloudflare issues it from Let's Encrypt **or** Google Trust Services, and can switch
  between them at renewal.
- The firmware therefore trusts a CA bundle (ISRG Root X1, ISRG Root X2, GTS Root R1 and
  GTS Root R4), not one pinned root. See the spec, §6.1.
- The tunnel from `cloudflared` back to the edge is encrypted by the tunnel itself. The hop
  from `cloudflared` to `inkboard:8000` is plain HTTP on the private Compose network.

## Checks

```bash
curl -s http://127.0.0.1:18440/healthz                       # local: {"fred": {...}, "weather": {...}}
curl -sI https://inkboard.signalwave.dev/v1/test.png | head -5   # public: HTTP/2 200, content-type image/png
curl -s -o /dev/null -w '%{http_code}\n' "https://inkboard.signalwave.dev/v1/frame.png?w=market_trends:1"  # 400
curl -sI "https://inkboard.signalwave.dev/v1/frame.png?w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles" \
  | grep -iE '^(cf-cache-status|etag|x-next-refresh-seconds)'
```

The last check must show `cf-cache-status: BYPASS` (or `DYNAMIC`), an `etag`, and
`x-next-refresh-seconds`. If it shows `HIT`, the cache rule from step 3 isn't matching.

## Moving the server

The tunnel token lives in `.env`, and Cloudflare routes to whichever connector is up. To move
to another machine:

1. Copy `server/.env` over.
2. Run `docker compose up -d --build` there.
3. Run `docker compose stop cloudflared` on the old machine, so the two don't split traffic.

The `cache` volume doesn't need to move; the server refills it within minutes.

## Rotating the token

In Zero Trust → Networks → Tunnels → `inkboard` → Configure, use **Refresh token** (or
delete and recreate the tunnel). Then update `CLOUDFLARE_TUNNEL_TOKEN` in `.env` and run
`docker compose up -d --force-recreate cloudflared`.
