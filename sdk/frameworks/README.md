<!-- Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ZXV framework kits

Enterprise framework integrations over the language SDKs. Each one wires the
same ideas into its framework's own idioms:

| Idea | [Spring Boot](spring-zxv/) | [ASP.NET Core](aspnetcore-zxv/) |
|---|---|---|
| One managed context, closed children first (ABI.md R6) | `ZxvContext` bean, named capabilities `depends-on` the root | hosted `ZxvPlatform`, container disposes in reverse creation order |
| Narrow capabilities from configuration; the root never injectable | `zxv.capabilities.<name>`, `@Qualifier("<name>")` | `Zxv:Capabilities:<name>`, `[FromKeyedServices("<name>")]` |
| Fail-closed health | Actuator `zxv`: DOWN when poisoned; liveness BROKEN | `AddHealthChecks().AddZxv()`: Unhealthy when halted |
| A halted context is never answered as a normal response | `ZxvFatalError` rethrown past every `@ExceptionHandler` (500) | middleware answers 503 and trips `ZxvFatalState` |
| Exact JSON | Jackson: `Rational` as `"0.1"` / `"1/3"`, JSON floats refused | System.Text.Json: same rules |
| Errors without secrets | (your `@ControllerAdvice`; the sample shows one) | problem details by status class, no native detail text |

| Kit | Package | Over | Tests |
|---|---|---|---|
| [`spring-zxv`](spring-zxv/) | `org.zedec:zxv-spring-boot-starter` | [`sdk/java`](../java/) (`org.zedec.zxv`) | 36 |
| [`aspnetcore-zxv`](aspnetcore-zxv/) | `Zxv.AspNetCore` | [`sdk/dotnet`](../dotnet/) (`Zxv.Sdk`) | 56 |

Both build with warnings as errors and test against the real `libzxv`.
CI: [`.github/workflows/sdk-frameworks.yml`](../../.github/workflows/sdk-frameworks.yml).
