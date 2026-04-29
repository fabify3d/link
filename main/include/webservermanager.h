#ifndef WEBSERVER_MANAGER_H
#define WEBSERVER_MANAGER_H

#include <esp_http_server.h>
#include <esp_log.h>
#include <cJSON.h>
#include <esp_http_client.h>
#include <string>
#include <cstring>
#include <cstdlib>

#include "commandprocessor.h"


class WebServerManager
{
private:
    httpd_handle_t server;
    static constexpr const char *TAG = "WebServer";
    bool isRunning;
    CommandProcessor *_cmdProcessor;

    // Small helper to safely send JSON error responses
    static void sendJsonError(httpd_req_t *req, const char *msg)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    }

    // HTML page for root endpoint
    static const char *getGreetingPage()
    {
        return R"%(
            <!DOCTYPE html>
            <html lang="en">
            <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
            <title>FabLink</title>
            <link rel="preconnect" href="https://fonts.googleapis.com">
            <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
            <link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&family=Poppins:wght@400;500;600;700&display=swap" rel="stylesheet">
            <style>
            :root {
            --green:#24ca70; --charcoal:#2f3842; --sky:#56cfe1;
            --orange:#FF8C42; --coral:#ff6b6b; --red:#dc362e;
            --grey:#b3b6bb; --surface:#38454f; --surface-deep:#28333d;
            --hl:linear-gradient(90deg,#24ca70,#56cfe1);
            --tap:48px;
            --r-sm:8px; --r-md:12px; --r-lg:16px; --r-xl:24px; --r-pill:9999px;
            --g-xs:4px; --g-sm:8px; --g-md:12px; --g-lg:16px; --g-xl:24px;
            }
            *,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
            html{-webkit-text-size-adjust:100%}
            button,input{font-family:inherit;border:none;outline:none}
            button{cursor:pointer;-webkit-tap-highlight-color:transparent;touch-action:manipulation}

            body{
            font-family:'Poppins',sans-serif;
            background:var(--charcoal);color:#fff;
            min-height:100dvh;display:flex;flex-direction:column;align-items:center;
            padding:var(--g-sm) 0 calc(var(--g-xl)*2 + env(safe-area-inset-bottom));
            gap:var(--g-sm);
            }
            body>*{width:92%;max-width:520px}

            /* ── HEADER ── */
            /* Wordmark · Cloud pill · [spacer] · Status · E-Stop · Settings
            At 280px: pill text truncates, Status shrinks, E-Stop & Settings stay full */
            header{
            display:flex;align-items:center;gap:var(--g-xs);
            min-height:var(--tap);padding:0 var(--g-md);
            background:rgba(40,51,61,0.92);
            backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);
            border:1px solid rgba(255,255,255,0.07);border-radius:var(--r-md);
            position:sticky;top:var(--g-sm);z-index:100;
            overflow:hidden;
            }
            .wordmark{
            font-size:0.95rem;font-weight:700;letter-spacing:-0.025em;
            color:#fff;text-decoration:none;line-height:1;flex-shrink:0;
            }
            .wordmark em{font-style:normal;color:var(--green);}

            .cloud-pill{
            font-family:'IBM Plex Mono',monospace;font-size:0.62rem;font-weight:600;
            padding:3px 8px;border-radius:var(--r-pill);
            border:1px solid rgba(255,255,255,0.1);background:rgba(255,255,255,0.04);
            color:var(--grey);white-space:nowrap;
            display:flex;align-items:center;gap:4px;
            transition:color .25s,border-color .25s,background .25s;line-height:1;
            flex-shrink:1;min-width:20px;overflow:hidden;
            }
            .cloud-pill::before{
            content:'';width:5px;height:5px;border-radius:50%;background:var(--grey);
            flex-shrink:0;transition:background .25s,box-shadow .25s;
            }
            .cloud-pill.live{color:var(--green);border-color:rgba(36,202,112,0.3);background:rgba(36,202,112,0.06);}
            .cloud-pill.live::before{background:var(--green);box-shadow:0 0 5px var(--green);}

            .hdr-spacer{flex:1;min-width:0;}

            .hbtn{
            height:var(--tap);min-width:var(--tap);padding:0 var(--g-sm);
            font-family:'Poppins',sans-serif;font-size:0.75rem;font-weight:600;
            color:rgba(255,255,255,0.65);background:transparent;border-radius:var(--r-sm);
            transition:background .15s,color .15s;
            display:flex;align-items:center;justify-content:center;flex-shrink:0;
            }
            .hbtn:hover,.hbtn:active{background:rgba(255,255,255,0.08);color:#fff;}

            /* E-STOP — always full width, never shrinks, permanently red */
            .estop-btn{
            height:var(--tap);padding:0 var(--g-md);
            font-family:'Poppins',sans-serif;font-size:0.75rem;font-weight:700;letter-spacing:0.02em;
            color:#fff;background:var(--red);border-radius:var(--r-sm);
            flex-shrink:0;display:flex;align-items:center;justify-content:center;
            transition:filter .1s,transform .08s;
            }
            .estop-btn:hover{filter:brightness(1.1);}
            .estop-btn:active{transform:scale(0.95);}
            .estop-btn.armed{
            background:#b71c1c;
            animation:ep 0.7s ease-in-out infinite alternate;
            }
            @keyframes ep{
            from{box-shadow:0 0 0 0 rgba(220,54,46,0.7);}
            to{box-shadow:0 0 0 7px rgba(220,54,46,0);}
            }

            /* ── BUTTONS ── */
            .btn{
            display:inline-flex;align-items:center;justify-content:center;
            height:var(--tap);padding:0 var(--g-lg);
            border-radius:var(--r-sm);font-family:'Poppins',sans-serif;font-weight:600;font-size:0.85rem;
            white-space:nowrap;transition:filter .15s,transform .1s,opacity .15s,background .15s;
            -webkit-tap-highlight-color:transparent;touch-action:manipulation;
            }
            .btn:hover:not(:disabled){filter:brightness(1.08);transform:translateY(-1px);}
            .btn:active:not(:disabled){transform:translateY(0);filter:brightness(0.95);}
            .btn:disabled{opacity:0.35;cursor:not-allowed;}
            .btn-primary{background:var(--green);color:var(--charcoal);}
            .btn-hl{background:var(--hl);color:var(--charcoal);}
            .btn-ghost{background:rgba(255,255,255,0.06);color:#fff;border:1px solid rgba(255,255,255,0.11);}
            .btn-ghost:hover:not(:disabled){background:rgba(255,255,255,0.1);filter:none;}
            .btn-danger{background:rgba(255,107,107,0.1);color:var(--coral);border:1px solid rgba(255,107,107,0.18);}
            .btn-danger:hover:not(:disabled){background:rgba(255,107,107,0.18);filter:none;}
            .btn-sm{height:36px;font-size:0.78rem;padding:0 12px;}
            .btn-full{width:100%;}

            /* ── UPLOAD ZONE ── */
            .upload-zone{
            display:flex;flex-direction:column;align-items:center;justify-content:center;
            gap:var(--g-sm);padding:var(--g-xl) var(--g-lg);
            background:var(--surface);border:1.5px dashed rgba(255,255,255,0.12);
            border-radius:var(--r-lg);cursor:pointer;text-align:center;
            transition:border-color .2s,background .2s;min-height:140px;
            }
            .upload-zone:hover,.upload-zone:focus-visible{
            border-color:var(--green);background:rgba(36,202,112,0.04);outline:none;
            }
            .upload-cta{font-size:0.95rem;font-weight:600;color:#fff;}
            .upload-cta span{color:var(--green);text-decoration:underline;text-decoration-color:rgba(36,202,112,0.4);text-underline-offset:3px;}
            .upload-hint{font-family:'IBM Plex Mono',monospace;font-size:0.7rem;color:var(--grey);}

            /* ── FILE STATUS ── */
            .file-panel{
            display:none;flex-direction:column;gap:var(--g-md);padding:var(--g-lg);
            background:var(--surface);border:1px solid rgba(255,255,255,0.07);border-radius:var(--r-lg);
            }
            .file-row{display:flex;justify-content:space-between;align-items:baseline;gap:var(--g-sm);min-width:0;}
            .file-name{font-family:'IBM Plex Mono',monospace;font-size:0.8rem;font-weight:500;color:var(--green);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1;min-width:0;}
            .file-pct{font-family:'IBM Plex Mono',monospace;font-size:0.78rem;font-weight:600;color:#fff;flex-shrink:0;}
            .prog-track{height:3px;background:rgba(255,255,255,0.08);border-radius:var(--r-pill);overflow:hidden;}
            .prog-fill{height:100%;width:0%;background:var(--hl);border-radius:var(--r-pill);transition:width .3s ease;}
            .file-eta{font-family:'IBM Plex Mono',monospace;font-size:0.68rem;color:var(--grey);}

            /* ── JOB CONTROLS ── */
            .act-secondary{display:flex;gap:var(--g-sm);}
            .act-secondary .btn{flex:1;}

            /* ── CARD (terminal) ── */
            .card{background:var(--surface);border:1px solid rgba(255,255,255,0.07);border-radius:var(--r-lg);overflow:hidden;}
            .card-hdr{display:flex;align-items:center;justify-content:space-between;padding:var(--g-md) var(--g-lg);border-bottom:1px solid rgba(255,255,255,0.06);}
            .card-label{font-family:'IBM Plex Mono',monospace;font-size:0.65rem;font-weight:600;letter-spacing:0.1em;text-transform:uppercase;color:var(--grey);}
            .card-tools{display:flex;align-items:center;gap:var(--g-sm);}
            .mode-badge{
            font-family:'IBM Plex Mono',monospace;font-size:0.65rem;font-weight:600;letter-spacing:0.04em;
            padding:3px 9px;border-radius:var(--r-pill);cursor:pointer;
            border:1px solid rgba(36,202,112,0.3);background:rgba(36,202,112,0.08);color:var(--green);
            transition:background .15s,color .15s,border-color .15s;
            }
            .mode-badge.raw{color:var(--orange);border-color:rgba(255,140,66,0.3);background:rgba(255,140,66,0.08);}

            /* ── TERMINAL ── */
            .term-card{display:none;flex-direction:column;}
            .term-out{
            font-family:'IBM Plex Mono',monospace;font-size:0.75rem;line-height:1.65;
            padding:var(--g-md) var(--g-lg);height:44dvh;overflow-y:auto;
            display:flex;flex-direction:column;gap:1px;
            scrollbar-width:thin;scrollbar-color:rgba(255,255,255,0.08) transparent;
            }
            .term-out::-webkit-scrollbar{width:3px;}
            .term-out::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.08);border-radius:2px;}
            .tl-info{color:var(--grey);} .tl-success{color:var(--green);} .tl-error{color:var(--coral);}
            .tl-estop{color:var(--red);font-weight:600;letter-spacing:0.05em;}
            .term-row{
            display:flex;align-items:center;gap:var(--g-sm);
            padding:var(--g-sm) var(--g-lg);
            border-top:1px solid rgba(255,255,255,0.06);background:var(--surface-deep);
            }
            .term-prompt{font-family:'IBM Plex Mono',monospace;font-size:0.85rem;font-weight:600;color:var(--green);user-select:none;flex-shrink:0;}
            .term-input{flex:1;background:transparent;font-family:'IBM Plex Mono',monospace;font-size:0.8rem;color:#fff;caret-color:var(--green);height:var(--tap);padding:0;}
            .term-input::placeholder{color:rgba(255,255,255,0.18);}

            /* ── OVERLAYS / SHEETS ── */
            .overlay{
            display:none;position:fixed;inset:0;
            background:rgba(0,0,0,0.6);backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px);
            z-index:200;align-items:flex-end;justify-content:center;
            }
            .sheet{
            width:100%;max-width:520px;background:var(--surface);
            border-radius:var(--r-xl) var(--r-xl) 0 0;border-top:1px solid rgba(255,255,255,0.1);
            max-height:92dvh;overflow-y:auto;
            animation:shUp .22s cubic-bezier(.32,1.2,.58,1);
            padding-bottom:calc(var(--g-lg) + env(safe-area-inset-bottom));
            }
            @keyframes shUp{from{transform:translateY(100%);opacity:0;}to{transform:translateY(0);opacity:1;}}
            .sh-handle{width:36px;height:4px;border-radius:var(--r-pill);background:rgba(255,255,255,0.14);margin:12px auto var(--g-xs);}
            .sh-hdr{display:flex;justify-content:space-between;align-items:center;padding:var(--g-md) var(--g-xl);}
            .sh-title{font-size:1rem;font-weight:700;letter-spacing:-0.01em;}
            .sh-close{
            height:var(--tap);min-width:var(--tap);display:flex;align-items:center;justify-content:center;
            font-family:'Poppins',sans-serif;font-size:0.78rem;font-weight:600;
            color:var(--grey);background:rgba(255,255,255,0.05);
            border:1px solid rgba(255,255,255,0.09);border-radius:var(--r-sm);
            transition:background .15s,color .15s;
            }
            .sh-close:hover{background:rgba(255,107,107,0.12);color:var(--coral);}
            .sh-body{padding:0 var(--g-xl) var(--g-xl);}
            .sh-body pre{
            font-family:'IBM Plex Mono',monospace;font-size:0.72rem;line-height:1.75;
            color:var(--green);background:var(--surface-deep);border-radius:var(--r-md);
            padding:var(--g-lg);white-space:pre-wrap;word-break:break-all;max-height:54dvh;overflow-y:auto;
            }

            /* ── SETTINGS SECTIONS ── */
            .settings-section{margin-bottom:var(--g-xl);}
            .settings-section:last-child{margin-bottom:0;}
            .section-title{
            font-family:'IBM Plex Mono',monospace;font-size:0.65rem;font-weight:600;
            letter-spacing:0.12em;text-transform:uppercase;color:var(--grey);
            margin-bottom:var(--g-md);padding-bottom:var(--g-sm);
            border-bottom:1px solid rgba(255,255,255,0.06);
            }

            /* ── FORM ── */
            .field{margin-bottom:var(--g-lg);}
            .field:last-child{margin-bottom:0;}
            .field-lbl{display:block;font-size:0.75rem;font-weight:600;color:var(--grey);letter-spacing:0.02em;margin-bottom:6px;}
            .field-hint{display:block;font-family:'IBM Plex Mono',monospace;font-size:0.65rem;color:rgba(179,182,187,0.6);margin-top:4px;line-height:1.5;}
            .field-wrap{position:relative;}
            .field-inp{
            width:100%;height:var(--tap);padding:0 var(--g-lg);
            background:var(--surface-deep);border:1.5px solid rgba(255,255,255,0.1);border-radius:var(--r-sm);
            color:#fff;font-family:'IBM Plex Mono',monospace;font-size:0.88rem;
            transition:border-color .2s,background .2s;
            }
            .field-inp:focus{border-color:var(--green);background:rgba(36,202,112,0.04);}
            .field-inp::placeholder{color:rgba(255,255,255,0.2);}
            .field-inp.pw{padding-right:calc(var(--tap) + 2px);}

            /* password toggle */
            .pw-toggle{
            position:absolute;right:0;top:0;height:var(--tap);width:var(--tap);
            display:flex;align-items:center;justify-content:center;
            background:transparent;border-radius:0 var(--r-sm) var(--r-sm) 0;
            font-family:'IBM Plex Mono',monospace;font-size:0.6rem;font-weight:600;
            letter-spacing:0.06em;text-transform:uppercase;color:var(--grey);
            transition:color .15s;user-select:none;
            }
            .pw-toggle:hover{color:#fff;}
            .pw-toggle:active{color:var(--green);}
            .pw-toggle[aria-pressed="true"]{
            color:var(--green);text-decoration:underline;
            text-underline-offset:2px;text-decoration-color:rgba(36,202,112,0.5);
            }

            /* WS live indicator */
            .ws-live{
            font-family:'IBM Plex Mono',monospace;font-size:0.68rem;
            display:flex;align-items:center;gap:5px;color:var(--grey);
            margin-top:var(--g-sm);
            }
            .ws-live::before{content:'';width:5px;height:5px;border-radius:50%;background:var(--grey);flex-shrink:0;transition:background .3s,box-shadow .3s;}
            .ws-live.live{color:var(--green);}
            .ws-live.live::before{background:var(--green);box-shadow:0 0 5px var(--green);}

            /* notices */
            .notice{display:none;padding:10px var(--g-md);border-radius:var(--r-sm);font-size:0.78rem;line-height:1.5;margin-top:var(--g-sm);}
            .notice.ok {display:block;background:rgba(36,202,112,0.1);color:var(--green);border:1px solid rgba(36,202,112,0.2);}
            .notice.err{display:block;background:rgba(255,107,107,0.1);color:var(--coral);border:1px solid rgba(255,107,107,0.2);}
            .notice.inf{display:block;background:rgba(86,207,225,0.07);color:var(--sky);border:1px solid rgba(86,207,225,0.2);}

            /* ── E-STOP CONFIRM SHEET ── */
            .estop-body{
            padding:var(--g-xl);display:flex;flex-direction:column;align-items:center;
            gap:var(--g-lg);text-align:center;
            }
            .estop-eyebrow{
            font-family:'IBM Plex Mono',monospace;font-size:0.65rem;font-weight:600;
            letter-spacing:0.14em;text-transform:uppercase;color:var(--red);margin-bottom:var(--g-xs);
            }
            .estop-title{font-size:1.2rem;font-weight:700;color:#fff;line-height:1.2;}
            .estop-sub{font-size:0.82rem;color:var(--grey);line-height:1.55;max-width:280px;}
            .estop-confirm{
            width:100%;height:56px;background:var(--red);color:#fff;
            font-family:'Poppins',sans-serif;font-size:1rem;font-weight:700;letter-spacing:0.02em;
            border-radius:var(--r-sm);transition:filter .1s,transform .08s;
            }
            .estop-confirm:hover{filter:brightness(1.1);}
            .estop-confirm:active{transform:scale(0.98);}
            .estop-dismiss{
            width:100%;height:var(--tap);
            background:rgba(255,255,255,0.06);color:rgba(255,255,255,0.6);
            font-family:'Poppins',sans-serif;font-size:0.85rem;font-weight:600;
            border:1px solid rgba(255,255,255,0.1);border-radius:var(--r-sm);
            transition:background .15s;
            }
            .estop-dismiss:hover{background:rgba(255,255,255,0.1);color:#fff;}

            /* ── FOOTER ── */
            footer{text-align:center;font-size:0.68rem;color:rgba(255,255,255,0.22);padding-top:var(--g-sm);}
            footer a{color:var(--green);text-decoration:none;}

            /* ── TABLET ── */
            @media(min-width:540px){
            .overlay{align-items:center;padding:var(--g-xl);}
            .sheet{border-radius:var(--r-xl);max-width:420px;border:1px solid rgba(255,255,255,0.1);}
            @keyframes shUp{from{opacity:0;transform:scale(.97) translateY(8px);}to{opacity:1;transform:scale(1) translateY(0);}}
            .sh-handle{display:none;}
            }
            </style>
            </head>
            <body>

            <!-- HEADER
                Wordmark · Cloud pill · [spacer] · Status · E-Stop · Settings
                Cloud pill text truncates via overflow:hidden at narrow widths;
                E-Stop and Settings are flex-shrink:0 so they never disappear.   -->
            <header>
            <a href="/" class="wordmark" aria-label="FabLink"><em>Fab</em>Link</a>
            <div class="cloud-pill" id="cloud-pill" aria-live="polite">Cloud</div>
            <div class="hdr-spacer"></div>
            <button class="hbtn" id="btn-status"   aria-label="System status">Status</button>
            <button class="hbtn" id="btn-settings" aria-label="Settings">Settings</button>
            <button class="estop-btn" id="btn-estop" aria-label="Emergency stop — opens confirmation">E-Stop</button>
            </header>

            <!-- UPLOAD ZONE -->
            <label class="upload-zone" id="upload-zone" for="file-input"
                tabindex="0" role="button" aria-label="Select a GCode file to send">
            <span class="upload-cta">Drop a file or <span>browse</span></span>
            <span class="upload-hint">.gcode · .nc · .txt</span>
            </label>
            <input type="file" id="file-input" style="display:none"
                accept=".gcode,.txt,.NC,.GCODE,.TXT,.nc">

            <!-- FILE STATUS -->
            <div class="file-panel" id="file-panel" aria-live="polite">
            <div class="file-row">
                <span class="file-name" id="file-name">—</span>
                <span class="file-pct"  id="file-pct">0%</span>
            </div>
            <div class="prog-track"><div class="prog-fill" id="prog-fill"></div></div>
            <span class="file-eta" id="file-eta"></span>
            </div>

            <!-- JOB CONTROLS — secondary row -->
            <div class="act-secondary">
            <button class="btn btn-danger btn-sm" id="btn-cancel" disabled>Cancel</button>
            <button class="btn btn-ghost  btn-sm" id="btn-resume" disabled>Resume</button>
            <button class="btn btn-ghost  btn-sm" id="btn-pause"  disabled>Pause</button>
            </div>

            <!-- PRIMARY SEND -->
            <button class="btn btn-hl btn-full" id="btn-send" disabled>Send file</button>

            <!-- TERMINAL TOGGLE -->
            <button class="btn btn-ghost btn-full" id="btn-term-toggle" aria-expanded="false">
            Open terminal
            </button>

            <!-- TERMINAL CARD -->
            <div class="card term-card" id="term-card">
            <div class="card-hdr">
                <span class="card-label">Terminal</span>
                <div class="card-tools">
                <span class="mode-badge" id="mode-badge" role="button" tabindex="0"
                        aria-label="Toggle GCode / Raw mode">GCode</span>
                <button class="btn btn-ghost btn-sm" id="btn-term-close">Close</button>
                </div>
            </div>
            <div class="term-out" id="term-out" role="log" aria-live="polite" aria-label="Terminal output"></div>
            <div class="term-row">
                <span class="term-prompt" aria-hidden="true">›</span>
                <input class="term-input" id="term-input" type="text"
                    placeholder="Enter GCode…"
                    autocomplete="off" autocorrect="off" autocapitalize="none"
                    spellcheck="false" inputmode="text" enterkeyhint="send"
                    aria-label="Command input">
                <button class="btn btn-primary btn-sm" id="btn-cmd">Send</button>
            </div>
            </div>

            <footer>Made with care by <a href="https://fabify.web.app" target="_blank" rel="noopener">Fabify</a></footer>


            <!-- ═══════════════════════ SHEETS ═══════════════════════ -->

            <!-- STATUS -->
            <div class="overlay" id="status-overlay" role="dialog" aria-modal="true" aria-label="System status">
            <div class="sheet">
                <div class="sh-handle"></div>
                <div class="sh-hdr">
                <span class="sh-title">System status</span>
                <button class="sh-close" id="status-close" aria-label="Close">Close</button>
                </div>
                <div class="sh-body"><pre id="status-pre">Loading…</pre></div>
            </div>
            </div>

            <!-- SETTINGS (WiFi + WS URI) -->
            <div class="overlay" id="settings-overlay" role="dialog" aria-modal="true" aria-label="Settings">
            <div class="sheet">
                <div class="sh-handle"></div>
                <div class="sh-hdr">
                <span class="sh-title">Settings</span>
                <button class="sh-close" id="settings-close" aria-label="Close settings">Close</button>
                </div>
                <div class="sh-body">

                <!-- WiFi section -->
                <div class="settings-section">
                    <div class="section-title">WiFi network</div>
                    <div class="field">
                    <label class="field-lbl" for="wifi-ssid">Network name (SSID)</label>
                    <div class="field-wrap">
                        <input class="field-inp" type="text" id="wifi-ssid"
                            placeholder="Your network name"
                            autocomplete="off" autocorrect="off" autocapitalize="none">
                    </div>
                    </div>
                    <div class="field">
                    <label class="field-lbl" for="wifi-pass">Password</label>
                    <div class="field-wrap">
                        <input class="field-inp pw" type="password" id="wifi-pass"
                            placeholder="Network password"
                            autocomplete="current-password">
                        <button class="pw-toggle" id="pw-toggle" type="button"
                                aria-label="Show password" aria-pressed="false">Show</button>
                    </div>
                    </div>
                    <div class="notice" id="wifi-notice"></div>
                    <button class="btn btn-primary btn-full" id="wifi-connect"
                            style="margin-top:var(--g-md)">Connect to WiFi</button>
                </div>

                <!-- FabOS server section -->
                <div class="settings-section">
                    <div class="section-title">FabOS server</div>
                    <div class="field">
                    <label class="field-lbl" for="ws-uri">WebSocket URI</label>
                    <div class="field-wrap">
                        <input class="field-inp" type="url" id="ws-uri"
                            placeholder="ws://192.168.1.x:4888/device"
                            autocomplete="off" autocorrect="off" autocapitalize="none"
                            spellcheck="false" inputmode="url">
                    </div>
                    <span class="field-hint">ws:// for local network · wss:// for cloud</span>
                    </div>
                    <div class="ws-live" id="ws-live-status">Checking…</div>
                    <div class="notice" id="ws-notice"></div>
                    <button class="btn btn-ghost btn-full" id="ws-connect"
                            style="margin-top:var(--g-md)">Connect to server</button>
                </div>

                </div>
            </div>
            </div>

            <!-- EMERGENCY STOP CONFIRMATION -->
            <div class="overlay" id="estop-overlay" role="alertdialog" aria-modal="true"
                aria-label="Emergency stop confirmation" aria-describedby="estop-desc">
            <div class="sheet">
                <div class="sh-handle"></div>
                <div class="estop-body">
                <div>
                    <div class="estop-eyebrow">Emergency stop</div>
                    <div class="estop-title">Halt all motion now?</div>
                </div>
                <div class="estop-sub" id="estop-desc">
                    Sends M112 immediately, bypassing the job queue.
                    The printer must be power-cycled to resume printing.
                </div>
                <button class="estop-confirm" id="btn-estop-confirm">
                    Confirm — stop machine
                </button>
                <button class="estop-dismiss" id="btn-estop-cancel">
                    Cancel
                </button>
                </div>
            </div>
            </div>


            <!-- ═══════════════════════ SCRIPT ═══════════════════════ -->
            <script>
            /* ── HELPERS ── */
            const $ = id => document.getElementById(id);
            const ts = () => new Date().toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit',second:'2-digit'});
            const sleep = ms => new Promise(r => setTimeout(r,ms));

            /* ── API ── */
            async function api(cmd){
            const r = await fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command:cmd})});
            const d = await r.json();
            if(!r.ok||d.status==='error') throw new Error(d.message||r.statusText);
            return d.result;
            }
            const gcode = cmd => api(`send ${cmd} --path=/com/s1 --id=ws`).catch(e=>`Error: ${e.message}`);

            /* ── TERMINAL ── */
            function tlog(text,type='info'){
            const out=$('term-out'), el=document.createElement('div');
            el.className='tl-'+type; el.textContent=text;
            out.appendChild(el); out.scrollTop=out.scrollHeight;
            while(out.children.length>600) out.removeChild(out.firstChild);
            }

            /* ── QUEUE ── */
            let queue=[],paused=false,busy=false,totalLines=0,sendStart=0,progressCb=null;

            async function runQueue(){
            if(busy||paused||!queue.length) return;
            busy=true;
            while(queue.length&&!paused){
                const line=queue.shift(), resp=await gcode(line);
                if(resp.startsWith('Error:')){
                tlog(`[${ts()}] ${line} → ${resp}`,'error');
                queue.unshift(line); paused=true; syncControls(); break;
                }
                tlog(`[${ts()}] ${line} → ${resp}`,'success');
                progressCb?.(totalLines-queue.length,totalLines);
                await sleep(50);
            }
            busy=false;
            if(!queue.length){tlog('File fully sent.','success'); resetFile();}
            }
            function loadFile(content,cb){
            progressCb=cb||null;
            queue=content.split(/\r?\n/).filter(l=>l.trim()&&!l.trim().startsWith(';'));
            totalLines=queue.length;
            tlog(`Queued ${totalLines} lines.`,'info');
            resumeQueue();
            }
            function pauseQueue() {paused=true; tlog('Paused.','info'); syncControls();}
            function resumeQueue(){if(!queue.length)return; paused=false; tlog('Resuming…','info'); syncControls(); runQueue();}
            function cancelQueue(){queue=[];paused=false;busy=false; tlog('Cancelled.','error'); resetFile();}
            function syncControls(){
            const has=queue.length>0;
            $('btn-cancel').disabled=!has;
            $('btn-pause').disabled=!has||paused;
            $('btn-resume').disabled=!has||!paused;
            }
            function resetFile(){
            $('btn-send').disabled=true;
            $('file-pct').textContent='0%';
            $('prog-fill').style.width='0%';
            $('file-eta').textContent='';
            $('file-name').textContent='—';
            $('file-panel').style.display='none';
            $('upload-zone').style.display='';
            $('file-input').value='';
            syncControls();
            }

            /* ── EMERGENCY STOP ── */
            // Step 1: header button → open confirm sheet (arms the button visually)
            $('btn-estop').addEventListener('click',()=>{
            $('btn-estop').classList.add('armed');
            openOverlay('estop-overlay');
            });

            // Cancel — disarm and close
            function disarmEstop(){
            $('btn-estop').classList.remove('armed');
            closeOverlay('estop-overlay');
            }
            $('btn-estop-cancel').addEventListener('click', disarmEstop);
            $('estop-overlay').addEventListener('click',e=>{if(e.target.id==='estop-overlay') disarmEstop();});

            // Step 2: confirmed — fire M112
            $('btn-estop-confirm').addEventListener('click', async()=>{
            disarmEstop();

            // Kill local queue instantly — no waiting for API
            queue=[]; paused=false; busy=false; syncControls();

            // Force terminal open so the operator can see the outcome
            if($('term-card').style.display!=='flex'){
                $('term-card').style.display='flex';
                $('btn-term-toggle').textContent='Close terminal';
                $('btn-term-toggle').setAttribute('aria-expanded','true');
                if(!$('term-out').children.length) tlog('FabLink terminal ready.','info');
            }

            tlog('! EMERGENCY STOP — sending M112','estop');

            try{
                // M112 bypasses the queue — sent as raw command directly
                const r = await api('send M112 --path=/com/s1 --id=ws');
                tlog('  M112 acknowledged: '+r,'estop');
            }catch(e){
                tlog('  M112 send error: '+e.message,'error');
                tlog('  Power-cycle the machine if motion has not stopped.','error');
            }

            resetFile();
            });

            /* ── CLOUD / WS POLL ── */
            async function pollCloud(){
            try{
                const r=await api('ws status');
                const live=r&&r.includes('Connected');
                const pill=$('cloud-pill');
                pill.className='cloud-pill'+(live?' live':'');
                pill.textContent=live?'Cloud · live':'Cloud';
                // update settings panel live indicator if open
                const li=$('ws-live-status');
                li.className='ws-live'+(live?' live':'');
                li.textContent=live?'Connected to server':'Not connected';
            }catch(_){}
            }
            pollCloud();
            setInterval(pollCloud,10000);

            /* ── OVERLAY HELPERS ── */
            function openOverlay(id) {$(id).style.display='flex'; document.body.style.overflow='hidden';}
            function closeOverlay(id){$(id).style.display='none';  document.body.style.overflow='';}

            /* ── STATUS SHEET ── */
            $('btn-status').addEventListener('click',async()=>{
            openOverlay('status-overlay');
            $('status-pre').textContent='Loading…';
            try{$('status-pre').textContent=await api('status');}
            catch(e){$('status-pre').textContent='Error: '+e.message;}
            });
            $('status-close').addEventListener('click',()=>closeOverlay('status-overlay'));
            $('status-overlay').addEventListener('click',e=>{if(e.target.id==='status-overlay') closeOverlay('status-overlay');});

            /* ── SETTINGS SHEET ── */
            $('btn-settings').addEventListener('click',()=>{
            // Reset form state on open
            $('wifi-ssid').value=''; $('wifi-pass').value='';
            $('wifi-pass').type='password';
            $('pw-toggle').textContent='Show';
            $('pw-toggle').setAttribute('aria-pressed','false');
            $('pw-toggle').setAttribute('aria-label','Show password');
            setNotice('wifi-notice','','');
            setNotice('ws-notice','','');
            pollCloud(); // refresh WS live indicator
            openOverlay('settings-overlay');
            setTimeout(()=>$('wifi-ssid').focus(),80);
            });
            $('settings-close').addEventListener('click',()=>closeOverlay('settings-overlay'));
            $('settings-overlay').addEventListener('click',e=>{if(e.target.id==='settings-overlay') closeOverlay('settings-overlay');});

            /* ── WIFI ── */
            $('wifi-connect').addEventListener('click',async()=>{
            const ssid=$('wifi-ssid').value.trim(), pass=$('wifi-pass').value, btn=$('wifi-connect');
            if(!ssid){setNotice('wifi-notice','err','Enter a network name (SSID).'); return;}
            btn.disabled=true; setNotice('wifi-notice','inf','Connecting…');
            try{
                const r=await api(`wifi connect "${ssid}" "${pass}"`);
                setNotice('wifi-notice','ok',r); tlog('WiFi: '+r,'success');
                setTimeout(()=>closeOverlay('settings-overlay'),2200);
            }catch(e){
                setNotice('wifi-notice','err',e.message); tlog('WiFi error: '+e.message,'error');
            }finally{btn.disabled=false;}
            });
            $('wifi-pass').addEventListener('keypress',e=>{if(e.key==='Enter') $('wifi-connect').click();});

            /* ── PASSWORD TOGGLE ── */
            $('pw-toggle').addEventListener('click',()=>{
            const inp=$('wifi-pass'), btn=$('pw-toggle');
            const revealing=inp.type==='password';
            inp.type=revealing?'text':'password';
            btn.textContent=revealing?'Hide':'Show';
            btn.setAttribute('aria-pressed',String(revealing));
            btn.setAttribute('aria-label',revealing?'Hide password':'Show password');
            inp.focus();
            const len=inp.value.length; inp.setSelectionRange(len,len);
            });

            /* ── WS URI ── */
            $('ws-connect').addEventListener('click',async()=>{
            const uri=$('ws-uri').value.trim(), btn=$('ws-connect');
            if(!uri){setNotice('ws-notice','err','Enter a WebSocket URI.'); return;}
            if(!uri.startsWith('ws://')&&!uri.startsWith('wss://')){
                setNotice('ws-notice','err','URI must start with ws:// or wss://'); return;
            }
            btn.disabled=true; setNotice('ws-notice','inf','Connecting…');
            try{
                const r=await api(`ws connect ${uri}`);
                setNotice('ws-notice','ok',r); tlog('WS server: '+r,'success');
                // allow connection to establish, then refresh cloud pill
                setTimeout(pollCloud,2500);
            }catch(e){
                setNotice('ws-notice','err',e.message); tlog('WS error: '+e.message,'error');
            }finally{btn.disabled=false;}
            });
            $('ws-uri').addEventListener('keypress',e=>{if(e.key==='Enter') $('ws-connect').click();});

            /* ── NOTICE HELPER ── */
            function setNotice(id,type,msg){
            const el=$(id);
            el.className='notice'+(type?' '+type:'');
            el.textContent=msg;
            }

            /* ── FILE INPUT ── */
            $('file-input').addEventListener('change',e=>{
            const f=e.target.files[0]; if(!f) return;
            $('upload-zone').style.display='none';
            $('file-name').textContent=f.name;
            $('file-panel').style.display='flex';
            $('btn-send').disabled=false;
            const reader=new FileReader();
            reader.onload=d=>{
                window.fileContent=d.target.result;
                const n=d.target.result.split(/\r?\n/).filter(Boolean).length;
                tlog(`Loaded "${f.name}" — ${n} lines. Ready to send.`,'info');
            };
            reader.readAsText(f);
            });

            $('btn-send').addEventListener('click',()=>{
            if(!window.fileContent) return;
            sendStart=Date.now();
            loadFile(window.fileContent,(sent,total)=>{
                const pct=((sent/total)*100).toFixed(1);
                $('file-pct').textContent=pct+'%';
                $('prog-fill').style.width=pct+'%';
                if(sent>0){
                const elapsed=Date.now()-sendStart, rem=(elapsed/sent)*(total-sent);
                $('file-eta').textContent=rem>60000
                    ?Math.ceil(rem/60000)+' min remaining'
                    :Math.ceil(rem/1000)+' s remaining';
                }
            });
            $('btn-send').disabled=true; syncControls();
            });

            $('btn-cancel').addEventListener('click',cancelQueue);
            $('btn-pause').addEventListener('click',pauseQueue);
            $('btn-resume').addEventListener('click',resumeQueue);

            /* ── TERMINAL ── */
            let gcodeMode=true;

            $('btn-term-toggle').addEventListener('click',()=>{
            const card=$('term-card'), open=card.style.display!=='flex';
            card.style.display=open?'flex':'none';
            $('btn-term-toggle').textContent=open?'Close terminal':'Open terminal';
            $('btn-term-toggle').setAttribute('aria-expanded',String(open));
            if(open){
                $('term-input').focus();
                if(!$('term-out').children.length) tlog('FabLink terminal ready.','info');
            }
            });

            $('btn-term-close').addEventListener('click',()=>{
            $('term-card').style.display='none';
            $('btn-term-toggle').textContent='Open terminal';
            $('btn-term-toggle').setAttribute('aria-expanded','false');
            });

            function toggleMode(){
            gcodeMode=!gcodeMode;
            const badge=$('mode-badge'), inp=$('term-input');
            badge.textContent=gcodeMode?'GCode':'Raw';
            badge.classList.toggle('raw',!gcodeMode);
            inp.placeholder=gcodeMode?'Enter GCode…':'Enter raw command…';
            tlog('Mode: '+(gcodeMode?'GCode':'Raw'),'info');
            }
            $('mode-badge').addEventListener('click',toggleMode);
            $('mode-badge').addEventListener('keypress',e=>{if(e.key===' '||e.key==='Enter') toggleMode();});

            async function sendCmd(){
            const inp=$('term-input'), cmd=inp.value.trim(); if(!cmd) return;
            tlog('› '+cmd,'info'); inp.value='';
            try{const r=gcodeMode?await gcode(cmd):await api(cmd); tlog('  '+r,'success');}
            catch(e){tlog('  Error: '+e.message,'error');}
            }
            $('btn-cmd').addEventListener('click',sendCmd);
            $('term-input').addEventListener('keypress',e=>{if(e.key==='Enter') sendCmd();});

            /* ── ESC ── */
            document.addEventListener('keydown',e=>{
            if(e.key==='Escape'){
                if($('estop-overlay').style.display==='flex') disarmEstop();
                else{closeOverlay('status-overlay'); closeOverlay('settings-overlay');}
            }
            });

            /* ── BOOT ── */
            tlog('FabLink ready.','info');
            </script>
            </body>
            </html>
        )%";
    }

    // Handler for root endpoint (does not require instance state but keeps consistent signature)
    static esp_err_t rootHandler(httpd_req_t *req)
    {
        const char *html = getGreetingPage();
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, html, strlen(html));
        ESP_LOGI(TAG, "Root page served");
        return ESP_OK;
    }

    // Handler for /cmd endpoint — uses user_ctx to access the instance
    static esp_err_t cmdHandler(httpd_req_t *req)
    {
        WebServerManager *self = reinterpret_cast<WebServerManager *>(req->user_ctx);
        if (!self)
        {
            sendJsonError(req, "{\"status\":\"error\",\"message\":\"Server context missing\"}");
            return ESP_OK;
        }

        if (!self->_cmdProcessor)
        {
            sendJsonError(req, "{\"status\":\"error\",\"message\":\"Command processor not set\"}");
            return ESP_OK;
        }

        int content_len = req->content_len;
        char *content = nullptr;
        int received = 0;

        if (content_len > 0)
        {
            // Allocate exact size + null
            content = static_cast<char *>(malloc(content_len + 1));
            if (!content)
            {
                sendJsonError(req, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
                return ESP_ERR_NO_MEM;
            }

            while (received < content_len)
            {
                int ret = httpd_req_recv(req, content + received, content_len - received);
                if (ret <= 0)
                {
                    // timeout or error
                    free(content);
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                    {
                        httpd_resp_send_408(req);
                        return ESP_FAIL;
                    }
                    sendJsonError(req, "{\"status\":\"error\",\"message\":\"Failed to read request body\"}");
                    return ESP_FAIL;
                }
                received += ret;
            }
            content[received] = '\0';
        }
        else
        {
            // No content length provided — attempt to read into a small buffer and expand dynamically.
            // This handles chunked transfers or unpredictable lengths.
            const size_t CHUNK = 512;
            size_t alloc_size = CHUNK;
            content = static_cast<char *>(malloc(alloc_size));
            if (!content)
            {
                sendJsonError(req, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
                return ESP_ERR_NO_MEM;
            }
            while (true)
            {
                int ret = httpd_req_recv(req, content + received, (int)(alloc_size - received - 1));
                if (ret > 0)
                {
                    received += ret;
                    if (alloc_size - received - 1 < (int)CHUNK)
                    {
                        // expand
                        alloc_size += CHUNK;
                        char *tmp = static_cast<char *>(realloc(content, alloc_size));
                        if (!tmp)
                        {
                            free(content);
                            sendJsonError(req, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
                            return ESP_ERR_NO_MEM;
                        }
                        content = tmp;
                    }
                    // continue reading until no more data
                    continue;
                }
                else if (ret == 0)
                {
                    // EOF
                    break;
                }
                else
                {
                    // ret < 0: timeout or error
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                    {
                        // Not fatal — break and try to parse what we have
                        break;
                    }
                    free(content);
                    sendJsonError(req, "{\"status\":\"error\",\"message\":\"Failed to read request body\"}");
                    return ESP_FAIL;
                }
            }
            content[received] = '\0';
        }

        ESP_LOGI(TAG, "Received command payload: %s", content);

        // Parse incoming JSON
        cJSON *root = cJSON_Parse(content);
        free(content); // free request buffer now
        if (root == nullptr)
        {
            sendJsonError(req, "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
            return ESP_OK;
        }

        // Get the command from JSON
        cJSON *cmd = cJSON_GetObjectItem(root, "command");
        if (cmd == nullptr || !cJSON_IsString(cmd))
        {
            cJSON_Delete(root);
            sendJsonError(req, "{\"status\":\"error\",\"message\":\"Missing 'command' field\"}");
            return ESP_OK;
        }

        const char *command = cmd->valuestring;
        ESP_LOGI(TAG, "Processing command: %s", command);

        // Delegate to command processor
        std::string results;
        try
        {
            results = self->_cmdProcessor->processCommand(std::string(command));
        }
        catch (...)
        {
            cJSON_Delete(root);
            sendJsonError(req, "{\"status\":\"error\",\"message\":\"Command processing failed\"}");
            return ESP_OK;
        }

        // Create response JSON
        cJSON *response = cJSON_CreateObject();
        if (!response)
        {
            cJSON_Delete(root);
            sendJsonError(req, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
            return ESP_OK;
        }

        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "result", results.c_str());
        cJSON_AddStringToObject(response, "message", "Command processed");

        char *response_str = cJSON_PrintUnformatted(response);
        if (!response_str)
        {
            cJSON_Delete(response);
            cJSON_Delete(root);
            sendJsonError(req, "{\"status\":\"error\",\"message\":\"Failed to create response\"}");
            return ESP_OK;
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response_str, strlen(response_str));

        // Cleanup
        free(response_str);
        cJSON_Delete(response);
        cJSON_Delete(root);

        return ESP_OK;
    }

public:
    WebServerManager() : server(nullptr), isRunning(false), _cmdProcessor(nullptr)
    {
        ESP_LOGI(TAG, "WebServerManager initialized");
    }

    ~WebServerManager()
    {
        stop();
    }

    void setCommandProcessor(CommandProcessor *processor)
    {
        _cmdProcessor = processor;
    }

    // Start the HTTP server
    bool start(uint16_t port = 80)
    {
        if (isRunning)
        {
            ESP_LOGW(TAG, "Server is already running");
            return true;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = port;
        // tune these as needed
        config.max_uri_handlers = 10;
        config.stack_size = 8192;

        ESP_LOGI(TAG, "Starting HTTP server on port %d", port);

        if (httpd_start(&server, &config) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start HTTP server");
            server = nullptr;
            return false;
        }

        // Register root endpoint
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = rootHandler,
            
            .user_ctx = this}; // not required for root, but set for consistency
        if (httpd_register_uri_handler(server, &root_uri) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to register root URI");
            // continue — server is still running
        }

        // Register /cmd endpoint
        httpd_uri_t cmd_uri = {
            .uri = "/cmd",
            .method = HTTP_POST,
            .handler = cmdHandler,
            .user_ctx = this};
        if (httpd_register_uri_handler(server, &cmd_uri) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register /cmd handler");
            httpd_stop(server);
            server = nullptr;
            return false;
        }

        isRunning = true;
        ESP_LOGI(TAG, "HTTP server started successfully");
        return true;
    }

    // Stop the HTTP server
    void stop()
    {
        if (!isRunning)
        {
            return;
        }

        if (server)
        {
            esp_err_t err = httpd_stop(server);
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "HTTP server stopped");
            }
            else
            {
                ESP_LOGW(TAG, "httpd_stop returned %s", esp_err_to_name(err));
            }
            server = nullptr;
        }
        isRunning = false;
    }

    // Check if server is running
    std::string getStatus() const
    {

        return "Running: " + std::string(isRunning ? "YES" : "NO") + "\n" +
               "Port: " + (isRunning ? "80" : "N/A") + "\n" +
               "Endpoints:\n" +
               (isRunning ? "  GET  /     - Greeting page\n  POST /cmd  - Command endpoint\n" : "  N/A\n");
    }

    bool running() const
    {
        return isRunning;
    }

    // Add custom endpoint (handler must be a free/static function)
    bool addEndpoint(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
    {
        if (!isRunning)
        {
            ESP_LOGE(TAG, "Cannot add endpoint: server is not running");
            return false;
        }

        httpd_uri_t new_uri = {
            .uri = uri,
            .method = method,
            .handler = handler,
            .user_ctx = this};

        if (httpd_register_uri_handler(server, &new_uri) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register URI: %s", uri);
            return false;
        }

        ESP_LOGI(TAG, "Endpoint added: %s", uri);
        return true;
    }

    // Send HTTP request to external server (basic helper with improved reading)
    // responseBuffer must be preallocated by caller with size bufferSize.
    bool sendRequest(const char *url, const char *method = "GET",
                     const char *postData = nullptr, char *responseBuffer = nullptr,
                     int bufferSize = 0)
    {
        if (!url)
            return false;

        esp_http_client_config_t config = {};
        config.url = url;
        config.timeout_ms = 5000;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr)
        {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            return false;
        }

        // Set method
        if (strcmp(method, "POST") == 0)
        {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            if (postData != nullptr)
            {
                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_post_field(client, postData, strlen(postData));
            }
        }
        else if (strcmp(method, "PUT") == 0)
        {
            esp_http_client_set_method(client, HTTP_METHOD_PUT);
        }
        else if (strcmp(method, "DELETE") == 0)
        {
            esp_http_client_set_method(client, HTTP_METHOD_DELETE);
        }
        else
        {
            esp_http_client_set_method(client, HTTP_METHOD_GET);
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return false;
        }

        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status = %d", status_code);

        // Read response if buffer provided
        if (responseBuffer != nullptr && bufferSize > 0)
        {
            int total_read = 0;
            while (total_read < bufferSize - 1)
            {
                int read_len = esp_http_client_read(client, responseBuffer + total_read, bufferSize - total_read - 1);
                if (read_len > 0)
                {
                    total_read += read_len;
                }
                else if (read_len == 0)
                {
                    // EOF
                    break;
                }
                else
                {
                    // error
                    ESP_LOGW(TAG, "Error while reading response: %d", read_len);
                    break;
                }
            }
            responseBuffer[total_read] = '\0';
            ESP_LOGI(TAG, "Response received: %d bytes", total_read);
        }

        esp_http_client_cleanup(client);
        return (status_code >= 200 && status_code < 300);
    }

    // Print server status
    void printStatus() const
    {
        ESP_LOGI(TAG, "=== Web Server Status ===");
        ESP_LOGI(TAG, "Running: %s", isRunning ? "YES" : "NO");
        if (isRunning)
        {
            ESP_LOGI(TAG, "Endpoints:");
            ESP_LOGI(TAG, "  GET  /     - Greeting page");
            ESP_LOGI(TAG, "  POST /cmd  - Command endpoint");
        }
        ESP_LOGI(TAG, "========================");
    }
};

#endif // WEBSERVER_MANAGER_H
