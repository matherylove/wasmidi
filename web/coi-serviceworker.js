/*
 * Minimal cross-origin-isolation service worker for GitHub Pages.
 * SnappySynthV2's adapted voice engine uses Emscripten pthreads, which require
 * SharedArrayBuffer. GitHub Pages cannot set COOP/COEP response headers, so the
 * service worker adds them to same-origin responses and reloads the page once.
 */
(() => {
    if (typeof window !== "undefined") {
        const alreadyIsolated = !!window.crossOriginIsolated;

        if (!("serviceWorker" in navigator)) {
            console.error("[WASMIDI] Service Worker API is unavailable; SnappySynthV2 pthreads cannot start.");
            return;
        }

        let reloading = false;
        navigator.serviceWorker.addEventListener("controllerchange", () => {
            if (reloading)
                return;
            reloading = true;
            window.location.reload();
        });

        // Never stop checking for SW updates merely because the *old* worker
        // already made this navigation cross-origin isolated. That behavior
        // could leave old deployment code controlling the site indefinitely.
        const current = document.currentScript && document.currentScript.src;
        const workerUrl = current
            ? new URL(current, window.location.href)
            : new URL("./coi-serviceworker.js", window.location.href);

        console.info(
            alreadyIsolated
                ? "[WASMIDI] cross-origin isolation active; checking deployment worker"
                : "[WASMIDI] preparing cross-origin isolation for SnappySynthV2");

        navigator.serviceWorker.register(workerUrl.href, {
            scope: "./",
            updateViaCache: "none"
        }).then(async registration => {
            try { await registration.update(); } catch (_) {}
            await navigator.serviceWorker.ready;

            if (!alreadyIsolated &&
                navigator.serviceWorker.controller &&
                !window.crossOriginIsolated) {
                const key = "wasmidi-coi-reload";
                if (sessionStorage.getItem(key) !== "1") {
                    sessionStorage.setItem(key, "1");
                    window.location.reload();
                }
            } else if (alreadyIsolated) {
                try { sessionStorage.removeItem("wasmidi-coi-reload"); } catch (_) {}
            }
        }).catch(error => {
            console.error("[WASMIDI] COI service worker registration failed:", error);
        });

        return;
    }

    self.addEventListener("install", () => self.skipWaiting());

    self.addEventListener("activate", event => {
        event.waitUntil(self.clients.claim());
    });

    self.addEventListener("fetch", event => {
        const request = event.request;

        if (request.cache === "only-if-cached" && request.mode !== "same-origin")
            return;

        const requestUrl = new URL(request.url);
        if (requestUrl.origin !== self.location.origin)
            return;

        event.respondWith((async () => {
            const path = requestUrl.pathname.toLowerCase();
            const freshnessCritical =
                request.mode === "navigate" ||
                /\.(?:js|wasm|html|data|json)$/.test(path) ||
                path.endsWith("/coi-serviceworker.js");

            // GitHub Pages may serve cacheable JS/WASM. For executable/runtime
            // assets always revalidate from the network so a successful deploy
            // cannot keep running yesterday's parser or Qt bootstrap.
            const response = await fetch(
                request,
                freshnessCritical ? { cache: "no-store" } : undefined);

            if (response.status === 0)
                return response;

            const headers = new Headers(response.headers);
            headers.set("Cross-Origin-Opener-Policy", "same-origin");
            headers.set("Cross-Origin-Embedder-Policy", "require-corp");
            headers.set("Cross-Origin-Resource-Policy", "same-origin");

            if (freshnessCritical) {
                headers.set("Cache-Control", "no-store, max-age=0");
                headers.set("Pragma", "no-cache");
            }

            return new Response(response.body, {
                status: response.status,
                statusText: response.statusText,
                headers
            });
        })());
    });
})();
