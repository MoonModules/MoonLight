---
hide:
  - navigation
  - toc
---

# REST API

Every HTTP route a device serves, with its parameters and answers, rendered from [openapi.yaml](openapi.yaml), the API's OpenAPI 3.1 description. A test holds it to the routes the server dispatches, both ways. How to use them together is in [Integrating](integrating.md).

<div id="redoc" style="background:#fff"></div>
<script src="https://cdn.jsdelivr.net/npm/redoc@2.5.4/bundles/redoc.standalone.js"></script>
<script>Redoc.init("openapi.yaml", { hideHostname: true }, document.getElementById("redoc"));</script>
