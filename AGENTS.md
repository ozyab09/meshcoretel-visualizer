# Repository Guidelines

## Project Structure & Module Organization
- `server.js` runs the Express + WebSocket backend.
- `public/` contains the client assets (e.g., `public/index.html`, images, scripts, styles).
- `package.json` defines runtime and dev dependencies plus npm scripts.
- `README.md` documents usage and API endpoints.

## Build, Test, and Development Commands
- `npm install` installs dependencies.
- `npm start` runs `server.js` with Node for a production-like run.
- `npm run dev` starts the server with `nodemon` for auto-reload during development.
- `npm test` is a placeholder and currently exits with an error.

## Coding Style & Naming Conventions
- JavaScript: prefer clear, small modules; keep logic close to where it is used.
- Indentation: 2 spaces, LF line endings.
- Filenames: lowercase with hyphens for new files; keep existing naming.
- Avoid reformatting unrelated code; keep diffs minimal.

## Testing Guidelines
- No formal test suite is configured.
- If you add tests, keep them fast and runnable with `npm test`, and document how to run them.
- Suggested naming pattern: `*.test.js` in a new `test/` directory.

## Commit & Pull Request Guidelines
- No strict commit convention is defined; use concise, imperative messages (e.g., `server: handle SSE reconnect`).
- PRs should include: purpose, summary of changes, manual verification steps, and risk notes.
- Link related issues when available and include screenshots only for UI changes.

## Security & Configuration Tips
- Do not commit secrets or tokens. Use environment variables for private values.
- This service talks to MeshCoreTel APIs; avoid logging sensitive payloads.

## Agent-Specific Instructions
- Keep changes focused, small, and reversible.
- Confirm before editing credentials or session artifacts.
