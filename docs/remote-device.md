# Driving a phone from a cloud session

A cloud session (Claude Code on the web) reaches the internet only through
an HTTPS proxy: no UDP, no inbound connections. It drives a phone over adb
through [tailcat](https://github.com/tailscale/tailcat), whose WireGuard
tunnel then rides a DERP relay over websockets. That tops out at about
0.15 MB/s per websocket, our own relay or tailcat's public ones alike:
the proxy is the limit (plain HTTPS through it moves 16 MB/s up), and a
phone reaches our relay at about 8 MB/s. An APK install takes minutes.
Two tailcats on one host find a direct path; measure the relay with
`TS_DEBUG_ALWAYS_USE_DERP=1`. A busy relay needs
`TS_DEBUG_DERP_PER_CLIENT_SEND_QUEUE_DEPTH=512` (the default 32 drops
packets on the slow websocket side).

```
cloud session ── tailcat (websocket, :443) ──► our derper ◄── tailcat ── phone: adbd
```

## The relay (once)

Any small server with a public IP and a TLS certificate will do (an Oracle
Cloud free AMD instance does).

- `derper` (`go install tailscale.com/cmd/derper@latest`; building on a
  1/8-core instance is slow, a static build from elsewhere copies over)
  as a service: `derper -hostname <name> -certmode manual -certdir <dir>
  -a :<port> -http-port -1 -stun-port 3478`, with `<dir>/<name>.crt` and
  `<name>.key` (symlinks to an ACME client's files work; restart weekly to
  pick up renewals).
- Firewall (ufw and the cloud's own rules): TCP `<port>`, UDP 3478.
- The cloud proxy only lets port 443 out. If 443 is taken on the server,
  forward 443 from the proxy's addresses to `<port>` (Caddy, iptables).
- Two DERP maps, the same relay as each side reaches it, served over HTTPS
  (a secret gist works):

```json
{"Regions":{"900":{"RegionID":900,"RegionCode":"own","RegionName":"own relay","Nodes":[
  {"Name":"900a","RegionID":900,"HostName":"<name>","DERPPort":<port>,"STUNPort":3478}]}}}
```

  The phone's map uses `<port>`, the cloud session's 443. A tailcat
  address names the region (900) or, with a saved key, none; never the
  port, so the two meet. connect.sh uses the cloud map whenever
  `WAYONDROID_DERPMAP_URL` is set. Don't give the phone `--full-address`:
  that bakes the phone's port in.

## Each session

```sh
scripts/remote/setup.sh                 # SDK and tailcat under build/remote-tools
export WAYONDROID_DERPMAP_URL=<the cloud side's DERP map>
scripts/remote/connect.sh <phone token> # adb on 127.0.0.1:5555
scripts/remote/deploy.sh                # build, install, launch
scripts/remote/hs.sh status <instance>  # the rootless helper's debug socket
scripts/remote/ui.py tap '<text>'       # tap by text or content description
```

These can live in the cloud environment's settings so new sessions start
with them:

- `WAYONDROID_DERPMAP_URL`: the cloud side's DERP map.
- `WAYONDROID_PHONE_TOKEN`: connect.sh's default token. A phone with a
  saved server key (`tailcat genkey --key=default`, once) keeps one
  address across restarts.
- `WAYONDROID_ADB_KEY` (a secret): an adb private key, PEM or base64 of
  it. setup.sh installs it, so the phone approves it once rather than
  once per session.

On the phone (Termux or similar, wireless debugging on, port fixed with
`adb tcpip 5555` or forwarded as is):

```sh
TAILCAT_DERPMAP_URL=<the phone's DERP map> tailcat serve 5555
```

The printed address is the token; after `tailcat genkey --key=default` it
stays the same. Keep the app running it out of battery optimization, or the
phone's low-memory killer may end it.

On a phone that may reboot, start both at boot from Termux:Boot
(`~/.termux/boot/tailcat`); `adb tcpip` doesn't survive a reboot, so a
rooted phone sets the port itself:

```sh
#!/data/data/com.termux/files/usr/bin/sh
termux-wake-lock
su -c 'setprop service.adb.tcp.port 5555; stop adbd; start adbd'
TAILCAT_DERPMAP_URL=<the phone's DERP map> tailcat serve 5555
```
