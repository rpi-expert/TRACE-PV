# TRACE-PV WebUI

The React frontend is served by `server.py`, a standard-library Python HTTP server that also starts simulator jobs. Build the simulator and databases first using the [root README](../README.md).

From the repository root:

```bash
(cd webui && npm ci && npm run build)
source setup_env.sh
TRACEPV_WEBUI_HOST=127.0.0.1 TRACEPV_WEBUI_PORT=8080 python3 webui/server.py
```

Open `http://localhost:8080`. For a remote host, forward port 8080 over SSH and open the same address locally. Keep the backend bound to localhost; the job-control API has no standalone authentication layer. Use a service manager such as supervisor for a persistent deployment.

On Vast images, Node may require `. /opt/nvm/nvm.sh` in noninteractive shells. The backend's `webui/build/` directory must exist or frontend requests return 404. Build assets are generated on the deployment host; do not copy `node_modules` between systems.

Simulator jobs run from the project root and write per-job logs under `webui/runs/`. A job marked started is not necessarily a completed simulation: inspect its status, progress and warnings. The default mission requires the full split CSVs; use the committed `tests/fixtures/deployment_mission.csv` for deployment checks, and limit iterations.

For frontend development, `npm start` runs the React development server. Its package proxy is `http://localhost:3001`; start the Python backend with `TRACEPV_WEBUI_PORT=3001` in that mode. Production uses the single Python server on port 8080.
