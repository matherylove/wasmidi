/*
 * Minimal cross-origin-isolation service worker for GitHub Pages.
 * SnappySynthV2's adapted voice engine uses Emscripten pthreads, which require
 * SharedArrayBuffer. GitHub Pages cannot set COOP/COEP response headers, so the
 * service worker adds them to same-origin responses and reloads the page once.
 */
(() => {
    if (typeof window !== "undefined") {
        if (window.crossOriginIsolated || !("serviceWorker" in navigator))
            return;

        let reloading = false;

        navigator.serviceWorker.addEventListener("controllerchange", () => {
            if (reloading)
                return;
            reloading = true;
            window.location.reload();
        });

        navigator.serviceWorker.register("./coi-serviceworker.js", {
            scope: "./"
        }).then(async registration => {
            await navigator.serviceWorker.ready;

            // If an older controller exists but this navigation was not
            // isolated, force exactly one new navigation through the SW.
            if (navigator.serviceWorker.controller && !window.crossOriginIsolated) {
                const key = "wasmidi-coi-reload";
                if (sessionStorage.getItem(key) !== "1") {
                    sessionStorage.setItem(key, "1");
                    window.location.reload();
                }
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

        event.respondWith((async () => {
            const response = await fetch(request);

            if (response.status === 0)
                return response;

            const headers = new Headers(response.headers);
            headers.set("Cross-Origin-Opener-Policy", "same-origin");
            headers.set("Cross-Origin-Embedder-Policy", "require-corp");
            headers.set("Cross-Origin-Resource-Policy", "same-origin");

            return new Response(response.body, {
                status: response.status,
                statusText: response.statusText,
                headers
            });
        })());
    });
})();
