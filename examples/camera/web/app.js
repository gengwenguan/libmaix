(() => {
    // ============================================================
    // Tab 切换
    // ============================================================
    const tabs  = document.getElementById('tabs');
    const panes = {
        live:     document.getElementById('pane-live'),
        playback: document.getElementById('pane-playback'),
        album:    document.getElementById('pane-album'),
        settings: document.getElementById('pane-settings'),
    };
    const deviceChip = document.getElementById('deviceChip');
    const toastStack = document.getElementById('toastStack');
    const confirmBackdrop = document.getElementById('confirmBackdrop');
    const confirmMessage = document.getElementById('confirmMessage');
    const confirmOk = document.getElementById('confirmOk');
    const confirmCancel = document.getElementById('confirmCancel');
    let curTab = 'live';
    let confirmResolver = null;
    for (const tab of tabs.querySelectorAll('.tab')) {
        tab.tabIndex = tab.classList.contains('active') ? 0 : -1;
    }

    function setChip(el, text, state) {
        if (!el) return;
        el.textContent = text;
        el.dataset.state = state || 'idle';
    }

    function showToast(message, type) {
        const item = document.createElement('div');
        item.className = 'toast' + (type === 'error' ? ' error' : '');
        item.textContent = message;
        toastStack.appendChild(item);
        setTimeout(() => item.remove(), type === 'error' ? 6000 : 3200);
    }

    function askConfirm(message) {
        if (confirmResolver) confirmResolver(false);
        confirmMessage.textContent = message;
        confirmBackdrop.classList.add('open');
        confirmBackdrop.setAttribute('aria-hidden', 'false');
        setTimeout(() => confirmOk.focus(), 0);
        return new Promise(resolve => { confirmResolver = resolve; });
    }

    function closeConfirm(result) {
        if (!confirmResolver) return;
        const resolve = confirmResolver;
        confirmResolver = null;
        confirmBackdrop.classList.remove('open');
        confirmBackdrop.setAttribute('aria-hidden', 'true');
        resolve(result);
    }

    confirmOk.addEventListener('click', () => closeConfirm(true));
    confirmCancel.addEventListener('click', () => closeConfirm(false));
    confirmBackdrop.addEventListener('click', ev => {
        if (ev.target === confirmBackdrop) closeConfirm(false);
    });
    document.addEventListener('keydown', ev => {
        if (ev.key !== 'Escape') return;
        if (confirmResolver) closeConfirm(false);
        const lightbox = document.getElementById('lightbox');
        if (lightbox && lightbox.classList.contains('open')) {
            closeLightbox();
        }
    });

    function activateTab(name) {
        if (!panes[name] || name === curTab) return;
        const activeTab = tabs.querySelector(`.tab[data-tab="${name}"]`);
        for (const el of tabs.querySelectorAll('.tab')) {
            const active = el === activeTab;
            el.classList.toggle('active', active);
            el.setAttribute('aria-selected', active ? 'true' : 'false');
            el.tabIndex = active ? 0 : -1;
        }
        for (const k of Object.keys(panes)) {
            const active = k === name;
            panes[k].classList.toggle('active', active);
            panes[k].hidden = !active;
        }
        curTab = name;
        if (name === 'playback' && !pbInited) initPlayback();
        if (name === 'album'    && !albumInited) initAlbum();
        if (name === 'album') refreshAlbum();
        if (name === 'settings' && !cfgInited) initSettings();
        setTimeout(() => syncLiveVisibility('tab'), 0);
    }

    tabs.addEventListener('click', ev => {
        const t = ev.target.closest('.tab');
        if (t) activateTab(t.getAttribute('data-tab'));
    });
    tabs.addEventListener('keydown', ev => {
        if (ev.key !== 'ArrowLeft' && ev.key !== 'ArrowRight') return;
        const items = Array.from(tabs.querySelectorAll('.tab'));
        const current = items.indexOf(document.activeElement);
        if (current < 0) return;
        ev.preventDefault();
        const step = ev.key === 'ArrowRight' ? 1 : -1;
        const next = items[(current + step + items.length) % items.length];
        next.focus();
        activateTab(next.getAttribute('data-tab'));
    });

    // ============================================================
    // 公共：API 封装
    // ============================================================
    async function api(method, path, body, timeoutMs) {
        const opt = { method, cache: 'no-store' };
        let timeout = null;
        if (typeof AbortController === 'function') {
            const controller = new AbortController();
            opt.signal = controller.signal;
            timeout = setTimeout(() => controller.abort(), timeoutMs || 8000);
        }
        if (body !== undefined) {
            opt.headers = { 'Content-Type': 'application/json' };
            opt.body = typeof body === 'string' ? body : JSON.stringify(body);
        }
        try {
            const r = await fetch(path, opt);
            const t = await r.text();
            try { return { ok: r.ok, status: r.status, data: JSON.parse(t) }; }
            catch(e) { return { ok: r.ok, status: r.status, data: t }; }
        } catch (e) {
            return {
                ok: false,
                status: 0,
                data: null,
                error: e && e.name === 'AbortError' ? '请求超时' : (e && e.message || '网络错误')
            };
        } finally {
            if (timeout) clearTimeout(timeout);
        }
    }
    function fmtSize(n) {
        if (n < 1024)            return n + ' B';
        if (n < 1024*1024)       return (n/1024).toFixed(1) + ' KB';
        if (n < 1024*1024*1024)  return (n/1024/1024).toFixed(2) + ' MB';
        return (n/1024/1024/1024).toFixed(2) + ' GB';
    }
    function dayPretty(yyyymmdd) {
        if (!yyyymmdd || yyyymmdd.length !== 8) return yyyymmdd || '';
        return yyyymmdd.slice(0,4) + '-' + yyyymmdd.slice(4,6) + '-' + yyyymmdd.slice(6,8);
    }
    function escapeHtml(value) {
        const map = { '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' };
        return String(value == null ? '' : value).replace(/[&<>"']/g, ch => map[ch]);
    }

    // 图片用 fetch + Blob 兼容自签名 HTTPS；MP4 可能达到数百 MiB，必须交给
    // 浏览器原生下载流，不能把整段录像先装进页面内存。
    async function downloadViaFetch(url, filename, onStatus) {
        try {
            if (onStatus) onStatus('下载中…');
            if (/\.mp4$/i.test(filename || url)) {
                const direct = document.createElement('a');
                direct.href = url;
                direct.download = filename || 'record.mp4';
                document.body.appendChild(direct);
                direct.click();
                document.body.removeChild(direct);
                if (onStatus) onStatus('已交给浏览器下载: ' + direct.download);
                return true;
            }
            const r = await fetch(url, { cache: 'no-store' });
            if (!r.ok) throw new Error('HTTP ' + r.status);
            const blob = await r.blob();
            const objUrl = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = objUrl;
            a.download = filename || 'download';
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            // 给浏览器一点时间发起保存，再回收 ObjectURL
            setTimeout(() => URL.revokeObjectURL(objUrl), 4000);
            if (onStatus) onStatus('已开始下载: ' + (filename || url));
            return true;
        } catch (e) {
            console.error('downloadViaFetch fail:', url, e);
            if (onStatus) onStatus('下载失败: ' + (e && e.message ? e.message : e));
            return false;
        }
    }

    // ============================================================
    // 实况 Tab：MSE 直播（与原版一致，仅去掉手动开始/停止录像按钮）
    // ============================================================
    const v       = document.getElementById('v');
    const stat    = document.getElementById('stat');
    const logBox  = document.getElementById('log');
    const btnLive = document.getElementById('btnLive');
    const btnUn   = document.getElementById('btnUnmute');
    const recDot  = document.getElementById('recDot');
    const recStat = document.getElementById('recStat');
    const netIpv4 = document.getElementById('netIpv4');
    const netIpv6 = document.getElementById('netIpv6');
    // 系统资源（/api/sysinfo）行
    const sysCpu = document.getElementById('sysCpu');
    const sysCpuBar = document.getElementById('sysCpuBar');
    const sysMem = document.getElementById('sysMem');
    const sysMemBar = document.getElementById('sysMemBar');
    const sysProc = document.getElementById('sysProc');
    const sysProcBar = document.getElementById('sysProcBar');
    const sysDisk = document.getElementById('sysDisk');
    const sysDiskBar = document.getElementById('sysDiskBar');
    const sysProcUptime = document.getElementById('sysProcUptime');
    const sysUptime = document.getElementById('sysUptime');

    // 本地实时时钟：渲染到画面右上角，用于和开发板烧录在画面左下角的
    // "采集时刻"肉眼比对，差值≈端到端直播延迟。
    // 格式与开发板烧录完全一致（YYYY-MM-DD HH:MM:SS），方便逐位对齐读差。
    // 秒级即可，按下一秒边界调度，避免 250ms 轮询造成无意义的移动端唤醒。
    const liveClock = document.getElementById('liveClock');
    (function tickClock() {
        const d  = new Date();
        const p2 = n => String(n).padStart(2, '0');
        liveClock.textContent =
            `${d.getFullYear()}-${p2(d.getMonth()+1)}-${p2(d.getDate())} ` +
            `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`;
        setTimeout(tickClock, 1020 - (Date.now() % 1000));
    })();

    // 直播 WebSocket 端口与协议对应关系（后端分别监听）：
    //   http://<host>/   →  ws://<host>:8081/   （明文 ws）
    //   https://<host>/  →  wss://<host>:8444/  （TLS wss）
    // 早期硬编码成 8081，导致从 https 进入时拼出 wss://host:8081 去敲
    // 一个明文 ws 端口，OpenSSL 没回应、浏览器直接报错，"实况" tab 永远连不上。
    const wsUrl = (() => {
        const isHttps = location.protocol === 'https:';
        const proto   = isHttps ? 'wss' : 'ws';
        const port    = isHttps ? 8444  : 8081;
        // location.hostname 对 IPv6 字面量会去掉方括号（如 2409:8a1e:...），
        // 直接拼 ws://2409:..:8081 会被解析成非法 URL。含 ':' 即为 IPv6，补回方括号。
        let host = location.hostname || '127.0.0.1';
        if (host.indexOf(':') !== -1 && host[0] !== '[') host = `[${host}]`;
        return `${proto}://${host}:${port}/`;
    })();

    // iPhone Safari 17.1+ 只暴露 ManagedMediaSource；桌面/Android 通常是
    // MediaSource，旧 WebKit 可能使用前缀版本。标准 MSE 可用时优先使用，
    // 否则回退到 MMS。
    const StandardMediaSourceCtor = window.MediaSource || window.WebKitMediaSource || null;
    const ManagedMediaSourceCtor = window.ManagedMediaSource || null;
    const MediaSourceCtor = StandardMediaSourceCtor || ManagedMediaSourceCtor;
    const isIosDevice = /iP(?:hone|ad|od)/.test(navigator.userAgent) ||
        (/Macintosh/.test(navigator.userAgent) && navigator.maxTouchPoints > 1);

    function isManagedMediaSourceCtor(ctor) {
        return !!(ManagedMediaSourceCtor && ctor === ManagedMediaSourceCtor);
    }

    function supportsMediaType(ctor, mime) {
        if (!ctor) return false;
        return typeof ctor.isTypeSupported !== 'function' || ctor.isTypeSupported(mime);
    }

    // Safari 在 iPhone 上使用 MMS 时要求把 blob URL 放在 <source> 子元素中，
    // 并禁用远程播放；普通 MSE 继续直接设置 video.src。
    function attachMediaSource(video, ctor) {
        const mediaSource = new ctor();
        const objectUrl = URL.createObjectURL(mediaSource);
        let sourceElement = null;
        video.disableRemotePlayback = true;
        if (isManagedMediaSourceCtor(ctor)) {
            video.removeAttribute('src');
            for (const old of video.querySelectorAll('source[data-mse-source]')) old.remove();
            sourceElement = document.createElement('source');
            sourceElement.dataset.mseSource = '1';
            sourceElement.type = 'video/mp4';
            sourceElement.src = objectUrl;
            video.appendChild(sourceElement);
            video.load();
        } else {
            video.src = objectUrl;
        }
        return { mediaSource, objectUrl, sourceElement };
    }

    function detachMediaSource(video, attachment) {
        if (!attachment) return;
        if (attachment.objectUrl) {
            try { URL.revokeObjectURL(attachment.objectUrl); } catch(e) {}
            attachment.objectUrl = '';
        }
        if (attachment.sourceElement) {
            try { attachment.sourceElement.remove(); } catch(e) {}
            attachment.sourceElement = null;
        }
        video.removeAttribute('src');
    }

    function waitForMediaSourceOpen(mediaSource) {
        if (mediaSource.readyState === 'open') return Promise.resolve(true);
        return new Promise(resolve => {
            let done = false;
            const finish = opened => {
                if (done) return;
                done = true;
                clearTimeout(timer);
                mediaSource.removeEventListener('sourceopen', onOpen);
                mediaSource.removeEventListener('webkitsourceopen', onOpen);
                resolve(opened);
            };
            const onOpen = () => finish(true);
            const timer = setTimeout(() => finish(false), 5000);
            mediaSource.addEventListener('sourceopen', onOpen, { once: true });
            mediaSource.addEventListener('webkitsourceopen', onOpen, { once: true });
        });
    }

    let ws = null, ms = null, sb = null;
    let queue = [], queueBytes = 0, appending = false;
    let gotInit = false, sourceOpen = false, liveInitBuf = null;
    let liveWanted = false;   // 用户是否"想看直播"：区分主动停 vs 切后台临时停
    let liveSession = 0;
    let liveAttachment = null;
    let reconnectTimer = null, reconnectAttempt = 0;
    let lastMediaAt = 0, lastVideoTime = -1, lastVideoProgressAt = 0;
    const LIVE_QUEUE_MAX_CHUNKS = 12;
    const LIVE_QUEUE_MAX_BYTES = 4 * 1024 * 1024;

    // 起播预缓冲：每秒一个 IDR、fragment ~1s，若拿到第 1 片就 seek 到末端并 play，
    // 缓冲只有 ~1s，播放头立刻贴边、readyState 不足 → 卡在首帧。改为先攒够
    // LIVE_START_BUFFER 秒再起播，起步即有水位，首帧顺滑。liveStarted 标记是否已起播。
    const LIVE_START_BUFFER = 1.8;
    const LIVE_TARGET_LATENCY = 1.0;
    let liveStarted = false;
    // 方案C：大 PTS 归零。板子 PTS 用单调时钟从进程启动累加，跑久了 fragment 的
    // baseMediaDecodeTime(tfdt) 会涨到几十万秒。新客户端中途接入时，MSE 时间轴从 0
    // 起却收到 tfdt=19万秒的 fragment → 解码基准错位 → 卡首帧/定格。
    // 解法：解析首个 media fragment 的 tfdt，在 append 前设 sb.timestampOffset
    // = -(tfdt/90000)，把前端时间轴拉回 ~0，与板子运行多久无关。
    const VIDEO_TIMESCALE = 90000;   // 与 fmp4Muxer kTbVideo90k 一致
    let tsOffsetSet = false;

    // 扫 MP4 box 找 moof/traf/tfdt，返回 baseMediaDecodeTime(90kHz)；找不到返回 null。
    // box 结构：[4B size][4B type][payload]。tfdt 在 moof>traf 内，full box：
    // [4B size][4B 'tfdt'][1B version][3B flags][4B 或 8B baseMediaDecodeTime]。
    function parseFirstTfdt(arrayBuf) {
        const dv = new DataView(arrayBuf);
        const len = dv.byteLength;
        // 在 [off, end) 范围内遍历同级 box，需要进入容器(moof/traf)就递归
        function scan(off, end) {
            while (off + 8 <= end) {
                const size = dv.getUint32(off);
                const type = String.fromCharCode(
                    dv.getUint8(off+4), dv.getUint8(off+5),
                    dv.getUint8(off+6), dv.getUint8(off+7));
                if (size < 8) return null;           // 防御非法 box
                const payloadStart = off + 8;
                const boxEnd = off + size;
                if (boxEnd > end) return null;
                if (type === 'moof' || type === 'traf') {
                    const r = scan(payloadStart, boxEnd); // 容器：进入
                    if (r !== null) return r;
                } else if (type === 'tfdt') {
                    const version = dv.getUint8(payloadStart);
                    const vOff = payloadStart + 4;       // 跳过 version(1)+flags(3)
                    if (version === 1) {
                        // 64-bit：高32位通常为0，用 getUint32 拼，避免 BigInt 兼容问题
                        const hi = dv.getUint32(vOff);
                        const lo = dv.getUint32(vOff + 4);
                        return hi * 4294967296 + lo;
                    } else {
                        return dv.getUint32(vOff);
                    }
                }
                off = boxEnd;
            }
            return null;
        }
        try { return scan(0, len); } catch(e) { return null; }
    }

    function topLevelBoxTypes(arrayBuf) {
        const dv = new DataView(arrayBuf);
        const types = [];
        let off = 0;
        try {
            while (off + 8 <= dv.byteLength) {
                let size = dv.getUint32(off);
                const type = String.fromCharCode(
                    dv.getUint8(off+4), dv.getUint8(off+5),
                    dv.getUint8(off+6), dv.getUint8(off+7));
                let header = 8;
                if (size === 1) {
                    if (off + 16 > dv.byteLength) break;
                    const hi = dv.getUint32(off + 8);
                    const lo = dv.getUint32(off + 12);
                    size = hi * 4294967296 + lo;
                    header = 16;
                } else if (size === 0) {
                    size = dv.byteLength - off;
                }
                if (size < header || off + size > dv.byteLength) break;
                types.push(type);
                off += size;
            }
        } catch(e) {}
        return types;
    }

    function isInitSegment(arrayBuf) {
        const types = topLevelBoxTypes(arrayBuf);
        return types.indexOf('ftyp') !== -1 && types.indexOf('moov') !== -1;
    }

    // avcC 中的 profile/compatibility/level 才是实际编码参数。动态生成 codec
    // 字符串，避免前端硬编码值与硬件编码器 SPS 不一致时被手机浏览器拒绝。
    function codecFromInitSegment(arrayBuf) {
        const dv = new DataView(arrayBuf);
        const hex = n => ('0' + n.toString(16)).slice(-2).toUpperCase();
        try {
            for (let i = 4; i + 8 < dv.byteLength; ++i) {
                if (dv.getUint8(i) === 0x61 && dv.getUint8(i+1) === 0x76 &&
                    dv.getUint8(i+2) === 0x63 && dv.getUint8(i+3) === 0x43 &&
                    dv.getUint8(i+4) === 1) {
                    return 'avc1.' + hex(dv.getUint8(i+5)) +
                           hex(dv.getUint8(i+6)) + hex(dv.getUint8(i+7));
                }
            }
        } catch(e) {}
        return 'avc1.4D001F';
    }

    function asArrayBuffer(data) {
        if (data instanceof ArrayBuffer) return Promise.resolve(data);
        if (ArrayBuffer.isView && ArrayBuffer.isView(data)) {
            return Promise.resolve(data.buffer.slice(
                data.byteOffset, data.byteOffset + data.byteLength));
        }
        if (typeof Blob !== 'undefined' && data instanceof Blob) {
            if (typeof data.arrayBuffer === 'function') return data.arrayBuffer();
            return new Promise((resolve, reject) => {
                const reader = new FileReader();
                reader.onload = () => resolve(reader.result);
                reader.onerror = () => reject(reader.error || new Error('Blob读取失败'));
                reader.readAsArrayBuffer(data);
            });
        }
        return Promise.reject(new Error('未知的WebSocket二进制类型'));
    }

    function enqueueLiveChunk(buf, init) {
        while (!init && queue.length > 0 &&
               (queue.length >= LIVE_QUEUE_MAX_CHUNKS ||
                queueBytes + buf.byteLength > LIVE_QUEUE_MAX_BYTES)) {
            const dropIndex = queue[0].init ? 1 : 0;
            if (dropIndex >= queue.length) break;
            queueBytes -= queue[dropIndex].buf.byteLength;
            queue.splice(dropIndex, 1);
        }
        if (buf.byteLength > LIVE_QUEUE_MAX_BYTES) {
            log('直播片段过大，已丢弃: ' + buf.byteLength + ' bytes');
            return;
        }
        queue.push({ buf, init: !!init });
        queueBytes += buf.byteLength;
    }

    function tailBufferedRange(sourceBuffer) {
        if (!sourceBuffer || !sourceBuffer.buffered || sourceBuffer.buffered.length === 0) {
            return null;
        }
        const i = sourceBuffer.buffered.length - 1;
        return { start: sourceBuffer.buffered.start(i), end: sourceBuffer.buffered.end(i) };
    }

    // 日志：限制最多保留 200 行，超出从顶部丢弃。
    // 原实现是 logBox.innerHTML += ... 无上限累加，长时间挂机看直播时
    // DOM 节点无限增长 → 页面自身内存膨胀、滚动变卡。改为 appendChild +
    // 超限删首节点，O(1) 控制规模。
    const LOG_MAX_LINES = 200;
    function log(msg) {
        const t = new Date().toLocaleTimeString();
        const line = document.createElement('div');
        line.textContent = `[${t}] ${msg}`;
        logBox.appendChild(line);
        while (logBox.childElementCount > LOG_MAX_LINES) {
            logBox.removeChild(logBox.firstChild);
        }
        logBox.scrollTop = logBox.scrollHeight;
    }
    function setStat(text, state) {
        let nextState = state;
        if (!nextState) {
            if (/播放中/.test(text)) nextState = 'playing';
            else if (/失败|错误|异常|不支持|中断/.test(text)) nextState = 'error';
            else if (/连接|等待|缓冲|暂停|重连/.test(text)) nextState = 'busy';
            else nextState = 'idle';
        }
        setChip(stat, text, nextState);
    }

    function syncLiveControls() {
        // 开始/停止合并成一个开关按钮：未在看→"开始直播"(青)，在看→"停止直播"(红)。
        const isLive = liveWanted;
        btnLive.textContent = isLive ? '停止直播' : '开始直播';
        btnLive.classList.toggle('live', isLive);
        // 未在看时，若 MSE 不可用则禁用；在看时可随时点停止。
        btnLive.disabled = !isLive && !MediaSourceCtor;
        btnUn.disabled = !MediaSourceCtor;
    }

    const MIME = 'video/mp4; codecs="avc1.4d001f, mp4a.40.2"';

    function attemptLivePlay() {
        try {
            const p = v.play();
            if (p && typeof p.catch === 'function') {
                p.catch(() => setStat('已缓冲，请点击画面播放'));
            }
        } catch(e) {
            setStat('已缓冲，请点击画面播放');
        }
    }

    function pump() {
        if (appending || !sb || sb.updating) return;
        if (queue.length === 0) return;
        const chunk = queue.shift();
        queueBytes -= chunk.buf.byteLength;
        // 方案C：在 append 第一个 media fragment 之前，按其 tfdt 设 timestampOffset，
        // 把大 PTS 拉回 ~0。init segment 无 tfdt（parseFirstTfdt 返回 null）跳过。
        // timestampOffset 只影响之后 append 的数据，故必须在首片 append 前设好。
        if (!chunk.init && !tsOffsetSet) {
            const tfdt = parseFirstTfdt(chunk.buf);
            if (tfdt !== null) {
                const off = -(tfdt / VIDEO_TIMESCALE);
                try {
                    sb.timestampOffset = off;
                    tsOffsetSet = true;
                    log(`tsOffset set: tfdt=${tfdt} -> offset=${off.toFixed(3)}s`);
                } catch(e) {
                    // 少数旧 WebKit 不允许设置 timestampOffset，sequence 模式仍可让
                    // 中途接入的片段从本地时间轴连续追加。
                    try {
                        sb.mode = 'sequence';
                        tsOffsetSet = true;
                        log('timestampOffset 不可用，切换 sequence 模式');
                    } catch(e2) {
                        log('直播时间轴初始化失败: ' + e.message);
                        scheduleReconnect('时间轴初始化失败');
                        return;
                    }
                }
            } else {
                log('收到不完整的 fMP4 fragment，准备重连');
                scheduleReconnect('片段格式异常');
                return;
            }
        }
        try {
            appending = true;
            sb.appendBuffer(chunk.buf);
        } catch (e) {
            appending = false;
            log('appendBuffer error: ' + e.message);
            scheduleReconnect('MSE追加失败');
        }
    }

    function attachSb(initBuf, session) {
        if (!sourceOpen || sb || !ms || session !== liveSession) return;

        const avcCodec = codecFromInitSegment(initBuf);
        const mimeCandidates = [
            `video/mp4; codecs="${avcCodec}, mp4a.40.2"`,
            MIME
        ];
        let sourceBuffer = null;
        let selectedMime = '';
        for (const mime of mimeCandidates) {
            if (selectedMime === mime) continue;
            try {
                if (!supportsMediaType(MediaSourceCtor, mime)) {
                    continue;
                }
                sourceBuffer = ms.addSourceBuffer(mime);
                selectedMime = mime;
                break;
            } catch(e) {}
        }
        if (!sourceBuffer) {
            log('当前浏览器不支持直播编码: ' + avcCodec + ' + AAC');
            setStat('浏览器不支持此直播编码');
            liveWanted = false;
            btnLive.disabled = true;
            teardownLive('浏览器不支持此直播编码');
            syncLiveControls();
            return;
        }

        sb = sourceBuffer;
        try { sourceBuffer.mode = 'segments'; } catch(e) {}
        log('SourceBuffer: ' + selectedMime);
        sourceBuffer.addEventListener('updateend', () => {
            if (sourceBuffer !== sb || session !== liveSession) return;
            appending = false;
            const range = tailBufferedRange(sourceBuffer);
            if (range) {
                const start = range.start;
                const end   = range.end;
                // 0) 起播预缓冲门槛：还没起播时，先攒够 LIVE_START_BUFFER 秒再 seek 到
                //    目标延迟约 1s，不能贴到 buffered.end，否则 1s 一片的离散输入会
                //    每片都耗尽缓冲，表现为首帧后反复 waiting。
                if (!liveStarted) {
                    if (end - start >= LIVE_START_BUFFER) {
                        const target = Math.max(start, end - LIVE_TARGET_LATENCY);
                        liveStarted = true;
                        lastVideoProgressAt = Date.now();
                        lastVideoTime = target;
                        log(`live start: buffered=${(end-start).toFixed(2)}s, seek -> ${target.toFixed(3)}`);
                        try { v.currentTime = target; } catch(e){}
                        try { v.playbackRate = 1.0; } catch(e){}
                        attemptLivePlay();
                    }
                    if (!liveStarted) {
                        pump();
                        return;
                    }
                }
                // 1) 修剪兜底：currentTime 已落到被 sb.remove 切掉的旧区段 → 跳到 buffered 头
                if (v.currentTime < start - 0.5) {
                    log('seek live: ' + v.currentTime.toFixed(3) + ' -> ' + start.toFixed(3));
                    try { v.currentTime = start; } catch(e){}
                    attemptLivePlay();
                }
                // 2) 低延时追尾：直播播放头落后 buffered 末端时，优先用"微调倍速"平滑追，
                //    避免 seek 触发解码重置的几十毫秒卡顿。只有落后过大（切后台 tab 回来等）
                //    倍速追太慢时才退化为硬 seek 拉平。
                //    触发场景（既覆盖启动也覆盖稳态）：
                //      - 起播：浏览器 MSE 收到 init+几个 fragment 后默认会攒 2-5s 才开播，
                //        currentTime 起步就远低于 buffered.end。
                //      - 切到非直播 tab：Chrome 在 background tab 里限速 video 解码，
                //        WebSocket → SourceBuffer 仍持续 append → buffered.end 飞涨，
                //        currentTime 龟速前进 → 切回 tab 时累积几十秒甚至几分钟延迟。
                //      - 网络抖动一次性涌入大量 fragment 同样会拉大差距。
                //    fMP4 约每秒到一片，因此稳定播放点需保留约 1s 前向水位；
                //    追得过近会在下一片抵达前耗尽缓冲并反复触发 waiting。
                else {
                    const lag = end - v.currentTime;
                    // 分级追尾：尽量用倍速平滑追，硬 seek 只作为"积压过大"的最后手段
                    // （硬 seek 会重置解码器，造成可见卡顿/花屏，是体感卡顿的主因之一）。
                    //   lag > 8s        → 积压过大，硬 seek 到 end-1s
                    //   lag > 2.5s      → 1.2x 快追
                    //   lag > 1.5s      → 1.08x 平滑追
                    //   lag <= 0.8s     → 恢复 1.0x，保留抗抖动水位
                    //   中间区为迟滞带，保持当前速度不抖动
                    if (lag > 8) {
                        const target = Math.max(start, end - LIVE_TARGET_LATENCY);
                        log(`live tail seek(big lag): ${v.currentTime.toFixed(3)} -> ${target.toFixed(3)} (lag=${lag.toFixed(2)}s)`);
                        try { v.currentTime = target; } catch(e){}
                        if (v.playbackRate !== 1.0) v.playbackRate = 1.0;
                        attemptLivePlay();
                    } else if (lag > 2.5) {
                        if (v.playbackRate !== 1.2) {
                            v.playbackRate = 1.2;
                            log(`live tail speed-up 1.2x (lag=${lag.toFixed(2)}s)`);
                        }
                    } else if (lag > 1.5) {
                        if (v.playbackRate !== 1.08) {
                            v.playbackRate = 1.08;
                            log(`live tail speed-up 1.08x (lag=${lag.toFixed(2)}s)`);
                        }
                    } else if (lag <= 0.8) {
                        if (v.playbackRate !== 1.0) {
                            v.playbackRate = 1.0;
                            log(`live tail back to 1.0x (lag=${lag.toFixed(2)}s)`);
                        }
                    }
                }
                // 3) 缓冲窗口压缩：直播不需要长历史，但留太薄(4s)抗抖动差、易触发追尾硬 seek。
                //    end-start > 12s 才修剪到留 6s，给网络抖动留足缓冲余量。
                if (end - start > 12) {
                    try { sourceBuffer.remove(start, end - 6); } catch(e){}
                }
            }
            pump();
        });
        sourceBuffer.addEventListener('error', () => {
            if (sourceBuffer !== sb || session !== liveSession) return;
            log('SourceBuffer error');
            scheduleReconnect('MSE解码异常');
        });
        pump();
    }

    function teardownLive(statusText) {
        ++liveSession;
        const oldWs = ws;
        ws = null;
        if (oldWs) {
            try { oldWs.onopen = oldWs.onmessage = oldWs.onerror = oldWs.onclose = null; } catch(e){}
            try { oldWs.close(); } catch(e){}
        }
        try { if (sb && sb.updating) sb.abort(); } catch(e){}
        try { v.pause(); } catch(e){}
        detachMediaSource(v, liveAttachment);
        liveAttachment = null;
        try { v.load(); } catch(e){}
        ms = null;
        sb = null;
        queue = [];
        queueBytes = 0;
        appending = false;
        gotInit = false;
        sourceOpen = false;
        liveInitBuf = null;
        liveStarted = false;
        tsOffsetSet = false;
        lastMediaAt = 0;
        lastVideoTime = -1;
        if (statusText) setStat(statusText);
    }

    function scheduleReconnect(reason) {
        if (!liveWanted || document.hidden || curTab !== 'live' ||
            !MediaSourceCtor || reconnectTimer) return;
        teardownLive();
        const delay = Math.min(8000, 500 * Math.pow(2, reconnectAttempt++));
        log(`${reason}，${delay}ms 后重连`);
        setStat('直播中断，正在重连…');
        reconnectTimer = setTimeout(() => {
            reconnectTimer = null;
            if (liveWanted && !document.hidden && curTab === 'live') startLive();
        }, delay);
        syncLiveControls();
    }

    function startLive() {
        if (ws) { log('已经在连接中'); return; }
        if (!MediaSourceCtor) {
            liveWanted = false;
            const reason = isIosDevice
                ? '当前 iOS 版本不支持直播，请升级至 iOS 17.1+'
                : '此浏览器不支持 MSE 直播';
            setStat(reason);
            log(reason + '；录像回放仍会尝试原生 MP4 路径');
            syncLiveControls();
            return;
        }
        if (reconnectTimer) {
            clearTimeout(reconnectTimer);
            reconnectTimer = null;
        }
        gotInit = false; sourceOpen = false; liveInitBuf = null;
        queue = []; queueBytes = 0;
        liveStarted = false;   // 复位起播门槛：本轮重新走预缓冲再起播
        tsOffsetSet = false;   // 复位时间戳偏移：本轮重新按首片 tfdt 拉回 0
        lastMediaAt = Date.now();
        try { v.playbackRate = 1.0; } catch(e){}   // 复位上一轮残留的追尾倍速
        const session = ++liveSession;
        let localMs;
        try {
            liveAttachment = attachMediaSource(v, MediaSourceCtor);
            localMs = liveAttachment.mediaSource;
            ms = localMs;
        } catch (e) {
            liveWanted = false;
            setStat('浏览器无法初始化媒体源', 'error');
            log('MediaSource 初始化/绑定失败: ' + (e && e.message || e));
            syncLiveControls();
            return;
        }
        waitForMediaSourceOpen(localMs).then(opened => {
            if (session !== liveSession || localMs !== ms) return;
            if (!opened) {
                scheduleReconnect('MediaSource 打开超时');
                return;
            }
            sourceOpen = true;
            if (liveAttachment && liveAttachment.objectUrl) {
                try { URL.revokeObjectURL(liveAttachment.objectUrl); } catch(e) {}
                liveAttachment.objectUrl = '';
            }
            log('MediaSource sourceopen');
            if (liveInitBuf) attachSb(liveInitBuf, session);
        });
        log('连接 ' + wsUrl);
        setStat('连接中…');
        syncLiveControls();
        const socket = new WebSocket(wsUrl);
        ws = socket;
        socket.binaryType = 'arraybuffer';
        socket.onopen = () => {
            if (session !== liveSession || socket !== ws) return;
            log('WebSocket 已连接');
            setStat('已连接，等待 init segment');
            setChip(deviceChip, location.hostname || '设备在线', 'online');
            try { socket.send(new Uint8Array([0xFF])); } catch(e){}
        };
        let messageChain = Promise.resolve();
        socket.onmessage = (ev) => {
            if (session !== liveSession || socket !== ws) return;
            if (typeof ev.data === 'string') { log('text msg: ' + ev.data); return; }
            messageChain = messageChain.then(() => asArrayBuffer(ev.data)).then((buf) => {
                if (session !== liveSession || socket !== ws) return;
                const init = isInitSegment(buf);
                if (!gotInit) {
                    if (!init) {
                        log('init segment 前收到媒体片段，已丢弃并请求关键帧');
                        try { socket.send(new Uint8Array([0xFF])); } catch(e){}
                        return;
                    }
                    gotInit = true;
                    liveInitBuf = buf;
                    setStat('收到 init segment，正在预缓冲');
                    log('init segment: ' + buf.byteLength + ' bytes');
                    enqueueLiveChunk(buf, true);
                    if (sourceOpen) attachSb(buf, session);
                } else {
                    if (!init) lastMediaAt = Date.now();
                    enqueueLiveChunk(buf, init);
                }
                pump();
            }).catch((e) => {
                if (session !== liveSession) return;
                log('WebSocket数据处理失败: ' + e.message);
                scheduleReconnect('直播数据异常');
            });
        };
        socket.onclose = () => {
            if (session !== liveSession || socket !== ws) return;
            ws = null;
            log('WebSocket 关闭');
            scheduleReconnect('连接关闭');
        };
        socket.onerror = () => {
            if (session !== liveSession || socket !== ws) return;
            log('WebSocket 错误');
            try { socket.close(); } catch(e){}
        };
    }

    function stopLive() {
        if (reconnectTimer) {
            clearTimeout(reconnectTimer);
            reconnectTimer = null;
        }
        reconnectAttempt = 0;
        teardownLive('已停止');
        syncLiveControls();
    }
    btnLive.onclick = () => {
        if (liveWanted) { liveWanted = false; stopLive(); }
        else { liveWanted = true; startLive(); syncLiveControls(); }
    };

    // 浏览器后台或切到其它功能页时主动释放 MSE/WebSocket，回到实况后自动重建。
    function syncLiveVisibility(reason) {
        const shouldRun = liveWanted && !document.hidden && curTab === 'live';
        if (shouldRun) {
            if (!ws && !reconnectTimer) startLive();
            else if (reason !== 'initial') liveCatchUpToTail(reason);
            return;
        }
        if (reconnectTimer) {
            clearTimeout(reconnectTimer);
            reconnectTimer = null;
        }
        if (ws || ms) {
            log(reason === 'tab' ? '切换功能页，暂停直播' : '页面隐藏，暂停直播');
            teardownLive(reason === 'tab' ? '切换页面，直播已暂停' : '页面已暂停');
        }
        syncLiveControls();
    }
    document.addEventListener('visibilitychange', () => {
        syncLiveVisibility('visibility');
        if (!document.hidden) refreshRecStatus();
    });
    // iOS Safari 进入后台、系统返回上一页或进入 BFCache 时不一定可靠触发
    // visibilitychange；pagehide/pageshow 再做一层生命周期兜底。
    window.addEventListener('pagehide', () => {
        if (reconnectTimer) {
            clearTimeout(reconnectTimer);
            reconnectTimer = null;
        }
        if (ws || ms) teardownLive('页面已暂停');
        syncLiveControls();
    });
    window.addEventListener('pageshow', event => {
        if (event.persisted || (!document.hidden && curTab === 'live')) {
            syncLiveVisibility('pageshow');
        }
    });
    // 静音按钮做成二态切换：跟随 video.muted 自动更新文案/图标
    function syncMuteBtn() {
        if (v.muted) { btnUn.textContent = '开启声音'; btnUn.title = '当前已静音，点击开启声音'; }
        else         { btnUn.textContent = '静音';     btnUn.title = '当前有声，点击静音'; }
    }
    btnUn.onclick = () => {
        v.muted = !v.muted;
        if (!v.muted) v.play().catch(()=>{});
        syncMuteBtn();
    };
    // 用户通过 video 原生控件改静音时也要同步按钮
    v.addEventListener('volumechange', syncMuteBtn);
    syncMuteBtn();
    v.addEventListener('waiting', () => setStat('缓冲中…'));
    v.addEventListener('playing', () => {
        setStat('播放中');
        reconnectAttempt = 0;
        lastVideoTime = v.currentTime;
        lastVideoProgressAt = Date.now();
    });
    v.addEventListener('click', () => {
        if (liveStarted && v.paused) attemptLivePlay();
    });

    // 无媒体数据或播放时间长时间不前进时主动重建 MSE。移动浏览器在网络切换、
    // 锁屏恢复后有时保持 WebSocket OPEN，却不再触发 SourceBuffer/解码事件。
    setInterval(() => {
        if (!liveWanted || document.hidden || curTab !== 'live' || !ws) return;
        const now = Date.now();
        if (lastMediaAt > 0 && now - lastMediaAt > 8000) {
            scheduleReconnect('超过8秒未收到媒体片段');
            return;
        }
        if (!liveStarted || v.paused) return;
        const cur = v.currentTime || 0;
        if (lastVideoTime < 0 || Math.abs(cur - lastVideoTime) > 0.05) {
            lastVideoTime = cur;
            lastVideoProgressAt = now;
        } else if (lastVideoProgressAt > 0 && now - lastVideoProgressAt > 6000) {
            scheduleReconnect('播放时间轴停滞');
        }
    }, 2000);

    // 切回直播 tab / 浏览器 tab 重新可见时，主动追尾。
    // 仅靠 SourceBuffer 'updateend' 是不够的：
    //   - 如果此时没有新 fragment 抵达（比如刚切回还没 updateend），就没人触发追平
    //   - background tab 期间累积的几十秒延时此时正等着被一把抹平
    function liveCatchUpToTail(why) {
        if (!sb || sb.updating || !v) return;
        const range = tailBufferedRange(sb);
        if (!range) return;
        const end = range.end;
        // 仅当积压较大（>3s，典型为长时间后台回来）才硬 seek 抹平；小幅落后交给
        // updateend 的倍速追尾平滑处理，避免每次切回前台都硬 seek 造成可见卡顿。
        if (end - (v.currentTime || 0) > 3.0) {
            const target = Math.max(range.start, end - LIVE_TARGET_LATENCY);
            log(`live tail catch-up (${why}): ${(v.currentTime||0).toFixed(3)} -> ${target.toFixed(3)}`);
            try { v.currentTime = target; } catch(e){}
            attemptLivePlay();
        }
    }
    // 拍照按钮：调 POST /api/snapshot；成功后给一行小提示并标记相册需要刷新
    const btnSnap  = document.getElementById('btnSnap');
    const snapTip  = document.getElementById('snapTip');
    btnSnap.onclick = async () => {
        btnSnap.disabled = true;
        snapTip.textContent = '正在拍照…';
        try {
            const r = await api('POST', '/api/snapshot');
            if (r.ok && r.data && r.data.ok) {
                snapTip.textContent =
                    `已保存：${r.data.name}（${fmtSize(r.data.size||0)}），可在相册查看`;
                albumDirty = true;
                showToast('照片已保存');
            } else {
                snapTip.textContent = '拍照失败：' +
                    (r.data && r.data.err || r.error || ('HTTP '+r.status));
                showToast(snapTip.textContent, 'error');
            }
        } catch (e) {
            snapTip.textContent = '拍照异常：' + e.message;
            showToast(snapTip.textContent, 'error');
        }
        btnSnap.disabled = false;
    };

    // 放门口提示音：调 POST /api/prompt {name:'door'}
    // 服务端同步阻塞约 1.4 秒（door.wav 时长）；并发调用返回 409 busy。
    // 提示音通过 C_TalkPlayer 共享 ALSA：talk 在线时与之串行；不在线时临时打开声卡。
    const btnDoor = document.getElementById('btnDoor');
    btnDoor.onclick = async () => {
        if (btnDoor.disabled) return;
        const orig = btnDoor.textContent;
        btnDoor.disabled = true;
        btnDoor.textContent = '播放中…';
        snapTip.textContent = '正在播放提示音「放门口」…';
        let r;
        try {
            r = await api('POST', '/api/prompt', { name: 'door' });
            if (r.ok && r.data && r.data.ok) {
                snapTip.textContent = '已播放提示音「放门口」'
                    + (r.data.duration_ms ? ` (${r.data.duration_ms}ms)` : '');
                showToast('提示音播放完成');
            } else if (r.status === 409) {
                snapTip.textContent = '提示音正在播放，稍后再试';
            } else if (r.status === 404) {
                snapTip.textContent = '提示音文件未部署：检查 dist/prompt/door.wav';
            } else {
                snapTip.textContent = '提示音失败：'
                    + ((r.data && r.data.err) || r.error || ('HTTP ' + r.status));
            }
            if (!r.ok) showToast(snapTip.textContent, 'error');
        } catch (e) {
            snapTip.textContent = '提示音异常：' + e.message;
            showToast(snapTip.textContent, 'error');
        }
        btnDoor.textContent = orig;
        btnDoor.disabled = false;
    };

    // ============================================================
    // 设备动作代理：web 上配置的"按钮 → URL"，点击后由后端出站 POST。
    //   GET  /api/actions            拉列表
    //   POST /api/actions            新增 {name,url}
    //   POST /api/actions/delete     删除 {id}
    //   POST /api/actions/invoke     触发 {id}（后端到目标 URL 发 POST）
    // 后端代理规避了浏览器跨域 / HTTPS 混合内容对局域网 http:// 设备的限制。
    // ============================================================
    const actionBtns  = document.getElementById('actionBtns');
    const actionEmpty = document.getElementById('actionEmpty');
    const actionTip   = document.getElementById('actionTip');
    const actionList  = document.getElementById('actionList');
    const actionName  = document.getElementById('actionName');
    const actionUrl   = document.getElementById('actionUrl');
    const actionAdd   = document.getElementById('actionAdd');
    const actionCancel = document.getElementById('actionCancel');
    let actionItems = [];
    let editingId = null;   // 非 null 时，表单处于"编辑"态（对应动作 id）

    function setActionTip(text, type) {
        actionTip.textContent = text || '';
        actionTip.dataset.state = type || 'idle';
    }

    function renderActions() {
        // 触发按钮区
        actionBtns.innerHTML = '';
        if (actionItems.length === 0) {
            actionEmpty.hidden = false;
            actionBtns.appendChild(actionEmpty);
        } else {
            actionEmpty.hidden = true;
            for (const it of actionItems) {
                const b = document.createElement('button');
                b.className = 'action-run';
                b.textContent = it.name;
                b.title = it.url;
                b.dataset.id = it.id;
                actionBtns.appendChild(b);
            }
        }
        // 管理列表
        actionList.innerHTML = '';
        for (const it of actionItems) {
            const li = document.createElement('li');
            if (it.id === editingId) li.classList.add('editing');
            li.innerHTML =
                `<div class="action-meta">
                    <span class="action-name">${escapeHtml(it.name)}</span>
                    <span class="action-url">${escapeHtml(it.url)}</span>
                 </div>
                 <div class="action-row-btns">
                    <button class="action-edit btn-quiet" data-id="${escapeHtml(it.id)}">编辑</button>
                    <button class="action-del btn-quiet" data-id="${escapeHtml(it.id)}">删除</button>
                 </div>`;
            actionList.appendChild(li);
        }
    }

    async function loadActions() {
        const r = await api('GET', '/api/actions?ts=' + Date.now());
        if (r.ok && r.data && r.data.ok && Array.isArray(r.data.actions)) {
            actionItems = r.data.actions;
            renderActions();
        } else if (r.status === 404) {
            // 后端版本尚不支持该接口：隐藏整卡，避免误导
            const card = document.getElementById('deviceActionCard');
            if (card) card.hidden = true;
        }
    }

    async function invokeAction(id, btn) {
        const it = actionItems.find(a => a.id === id);
        const label = it ? it.name : '动作';
        if (btn) btn.disabled = true;
        setActionTip(`正在执行「${label}」…`, 'busy');
        try {
            const r = await api('POST', '/api/actions/invoke', { id });
            if (r.ok && r.data && r.data.ok) {
                setActionTip(`「${label}」已发送 · 目标返回 ${r.data.status}`, 'playing');
            } else {
                const err = (r.data && r.data.err) || r.error || ('HTTP ' + r.status);
                setActionTip(`「${label}」失败：${err}`, 'error');
                showToast(`设备动作「${label}」失败：${err}`, 'error');
            }
        } catch (e) {
            setActionTip(`「${label}」异常：${e.message}`, 'error');
        }
        if (btn) btn.disabled = false;
    }

    // 进入编辑态：把某条动作的名称/URL 填回表单，按钮文案切到"保存"并露出"取消"。
    function enterEditMode(id) {
        const it = actionItems.find(a => a.id === id);
        if (!it) return;
        editingId = id;
        actionName.value = it.name;
        actionUrl.value  = it.url;
        actionAdd.textContent = '保存';
        actionCancel.hidden = false;
        setActionTip(`正在编辑「${it.name}」`, 'busy');
        renderActions();          // 高亮当前编辑行
        actionName.focus();
    }

    // 退出编辑态：清空表单并恢复"添加"外观。
    function exitEditMode() {
        editingId = null;
        actionName.value = '';
        actionUrl.value  = '';
        actionAdd.textContent = '添加';
        actionCancel.hidden = true;
        renderActions();
    }

    // 表单提交：editingId 为空走新增，否则走更新。
    async function submitAction() {
        const name = (actionName.value || '').trim();
        const url  = (actionUrl.value  || '').trim();
        if (!name) { setActionTip('请填写按钮名', 'error'); actionName.focus(); return; }
        if (!/^http:\/\/.+/i.test(url)) {
            setActionTip('URL 必须以 http:// 开头', 'error'); actionUrl.focus(); return;
        }
        const isEdit = !!editingId;
        actionAdd.disabled = true;
        setActionTip(isEdit ? '正在保存…' : '正在添加…', 'busy');
        try {
            const r = isEdit
                ? await api('POST', '/api/actions/update', { id: editingId, name, url })
                : await api('POST', '/api/actions', { name, url });
            if (r.ok && r.data && r.data.ok) {
                exitEditMode();
                setActionTip(isEdit ? '已保存' : '已添加', 'playing');
                await loadActions();
            } else {
                const err = (r.data && r.data.err) || r.error || ('HTTP ' + r.status);
                setActionTip((isEdit ? '保存失败：' : '添加失败：') + err, 'error');
            }
        } catch (e) {
            setActionTip((isEdit ? '保存异常：' : '添加异常：') + e.message, 'error');
        }
        actionAdd.disabled = false;
    }

    async function deleteAction(id, btn) {
        const it = actionItems.find(a => a.id === id);
        if (!await askConfirm(`确定删除动作「${it ? it.name : id}」吗？`)) return;
        if (btn) btn.disabled = true;
        try {
            const r = await api('POST', '/api/actions/delete', { id });
            if (r.ok && r.data && r.data.ok) {
                if (editingId === id) exitEditMode();   // 正在编辑的被删掉，退出编辑态
                setActionTip('已删除', 'idle');
                await loadActions();
            } else {
                const err = (r.data && r.data.err) || r.error || ('HTTP ' + r.status);
                setActionTip('删除失败：' + err, 'error');
                if (btn) btn.disabled = false;
            }
        } catch (e) {
            setActionTip('删除异常：' + e.message, 'error');
            if (btn) btn.disabled = false;
        }
    }

    // 事件委托：触发按钮 + 编辑/删除按钮
    actionBtns.addEventListener('click', (ev) => {
        const b = ev.target.closest('.action-run');
        if (b && b.dataset.id) invokeAction(b.dataset.id, b);
    });
    actionList.addEventListener('click', (ev) => {
        const edit = ev.target.closest('.action-edit');
        if (edit && edit.dataset.id) { enterEditMode(edit.dataset.id); return; }
        const del = ev.target.closest('.action-del');
        if (del && del.dataset.id) deleteAction(del.dataset.id, del);
    });
    actionAdd.addEventListener('click', submitAction);
    actionCancel.addEventListener('click', exitEditMode);
    actionUrl.addEventListener('keydown', (ev) => {
        if (ev.key === 'Enter') { ev.preventDefault(); submitAction(); }
    });
    loadActions();


    // ============================================================
    // 设备运行日志：订阅后端 /ws/log，实时显示 camera 进程的 CLOG_* 输出。
    //   - 折叠框，默认收起；仅在展开时才建立 WebSocket，收起即断开，省流量/内存。
    //   - 后端用 binary 帧承载 UTF-8 文本；这里 TextDecoder 解码后按行渲染。
    //   - 最多保留 100 行，超限删最早；按日志级别（ERR/FLT/WRN/INF）染色。
    // 复用直播 WS 的 host/协议推导：ws↔wss、IPv6 字面量补方括号，端口同直播口。
    // ============================================================
    const devLogBox   = document.getElementById('deviceLogBox');
    const devLogEl    = document.getElementById('devLog');
    const devLogState = document.getElementById('devLogState');
    const DEV_LOG_MAX_LINES = 100;
    const devLogUrl = wsUrl.replace(/\/$/, '') + '/ws/log';
    let devLogWs = null;
    let devLogDecoder = (typeof TextDecoder !== 'undefined') ? new TextDecoder('utf-8') : null;
    let devLogPartial = '';   // 跨帧残行拼接（正常一帧一行，这里做兜底）

    function setDevLogState(text) { if (devLogState) devLogState.textContent = text ? ' · ' + text : ''; }

    // 从整行里判定级别：格式为 "... INF:[ ... ]<file:line:func>: msg"。
    function devLogLevel(line) {
        const m = /\s(FLT|ERR|WRN|INF):\[/.exec(line);
        return m ? m[1] : '';
    }

    function appendDevLog(line) {
        const text = line.replace(/\s+$/, '');
        if (!text) return;
        const div = document.createElement('div');
        div.className = 'dev-log-line';
        const lv = devLogLevel(text);
        if (lv === 'ERR' || lv === 'FLT') div.classList.add('lv-err');
        else if (lv === 'WRN') div.classList.add('lv-wrn');
        else if (lv === 'INF') div.classList.add('lv-inf');
        div.textContent = text;
        // 接近底部时才自动滚动，避免用户上翻查看时被强行拽回底部。
        const atBottom = devLogEl.scrollHeight - devLogEl.scrollTop - devLogEl.clientHeight < 24;
        devLogEl.appendChild(div);
        while (devLogEl.childElementCount > DEV_LOG_MAX_LINES) {
            devLogEl.removeChild(devLogEl.firstChild);
        }
        if (atBottom) devLogEl.scrollTop = devLogEl.scrollHeight;
    }

    function feedDevLogChunk(str) {
        devLogPartial += str;
        let idx;
        while ((idx = devLogPartial.indexOf('\n')) !== -1) {
            appendDevLog(devLogPartial.slice(0, idx));
            devLogPartial = devLogPartial.slice(idx + 1);
        }
        // 后端每条日志自带结尾换行；若一帧无换行则先缓存，等待下一帧或关闭时冲刷。
        if (devLogPartial.length > 4096) { appendDevLog(devLogPartial); devLogPartial = ''; }
    }

    function openDevLog() {
        if (devLogWs) return;
        setDevLogState('连接中');
        let socket;
        try { socket = new WebSocket(devLogUrl); }
        catch (e) { setDevLogState('连接失败'); return; }
        devLogWs = socket;
        socket.binaryType = 'arraybuffer';
        socket.onopen = () => { if (socket === devLogWs) setDevLogState('已连接'); };
        socket.onmessage = (ev) => {
            if (socket !== devLogWs) return;
            let str = '';
            if (typeof ev.data === 'string') str = ev.data;
            else if (devLogDecoder) str = devLogDecoder.decode(new Uint8Array(ev.data));
            if (str) feedDevLogChunk(str);
        };
        socket.onclose = () => {
            if (socket !== devLogWs) return;
            devLogWs = null;
            // 仍处于展开态才提示断开（收起触发的主动关闭不提示）。
            if (devLogBox.open) setDevLogState('已断开');
        };
        socket.onerror = () => { try { socket.close(); } catch (e) {} };
    }

    function closeDevLog() {
        const socket = devLogWs;
        devLogWs = null;
        if (socket) { try { socket.close(); } catch (e) {} }
        devLogPartial = '';
        setDevLogState('');
    }

    if (devLogBox) {
        devLogBox.addEventListener('toggle', () => {
            if (devLogBox.open) openDevLog();
            else closeDevLog();
        });
    }
    // 切走实况页 / 页面隐藏时释放日志连接；回到实况且仍展开时重连。
    document.addEventListener('visibilitychange', () => {
        if (document.hidden) closeDevLog();
        else if (devLogBox && devLogBox.open && curTab === 'live') openDevLog();
    });


    async function refreshRecStatus() {
        if (document.hidden) return;
        const r = await api('GET', '/api/record/status?ts=' + Date.now());
        if (!r.ok) {
            recDot.classList.remove('on');
            recStat.textContent = '状态获取失败';
            setChip(deviceChip, (location.hostname || '设备') + ' · 离线', 'error');
            return;
        }
        setChip(deviceChip, location.hostname || '设备在线', 'online');
        const s = r.data || {};
        if (s.recording) {
            recDot.classList.add('on');
            recStat.textContent = `录像中 · ${s.file || ''} · ${fmtSize(s.bytes||0)}`;
        } else {
            recDot.classList.remove('on');
            recStat.textContent = '录像未启动';
        }
    }
    setInterval(refreshRecStatus, 5000);
    refreshRecStatus();

    // 设备网卡地址（真实 IPv4 / IPv6，来自板端枚举，而非浏览器访问用的 host）。
    // 地址变化不频繁，30s 轮询一次即可；拿不到 IPv6 时显示占位而不是空白。
    let lastNetInfoError = '';
    async function refreshNetInfo() {
        if (document.hidden) return;
        const r = await api('GET', '/api/netinfo?ts=' + Date.now());
        if (!r.ok || !r.data || typeof r.data !== 'object') {
            const accessHost = location.hostname || '';
            const accessIsIpv4 = /^(?:\d{1,3}\.){3}\d{1,3}$/.test(accessHost);
            if (r.status === 404) {
                // 新前端配旧二进制时接口不存在。IPv4 可用浏览器当前访问地址临时兜底，
                // 但它不一定等于设备首选网卡地址，因此明确标注来源。
                netIpv4.textContent = accessIsIpv4
                    ? accessHost + '（访问地址）'
                    : '后端版本不支持';
                netIpv6.textContent = '后端版本不支持';
            } else {
                netIpv4.textContent = accessIsIpv4
                    ? accessHost + '（访问地址）'
                    : '获取失败';
                netIpv6.textContent = '获取失败';
            }
            const reason = r.status === 404
                ? '/api/netinfo 不存在，请更新后端二进制'
                : '网络地址获取失败：' + (r.error || ('HTTP ' + r.status));
            if (reason !== lastNetInfoError) {
                log(reason);
                lastNetInfoError = reason;
            }
            return;
        }
        lastNetInfoError = '';
        const v4 = r.data.ipv4 && r.data.ipv4 !== '0.0.0.0' ? r.data.ipv4 : '';
        const v6 = r.data.ipv6 || '';
        netIpv4.textContent = v4 || '暂无';
        netIpv6.textContent = v6 || '暂无（未分配全局地址）';
    }
    setInterval(refreshNetInfo, 30000);
    refreshNetInfo();

    // ============================================================
    // 系统资源（CPU / 内存 / 进程内存 / 存储 / 运行时长）
    // 5 秒轮询 /api/sysinfo。每项带一条百分比进度条，按水位染色（<70 正常、
    // 70~90 偏高、>90 危险）。进程内存对齐 mem_watchdog 阈值显示"离重启多远"。
    // 后端旧版本无该接口时（404）整体标注"后端版本不支持"，不刷屏。
    // ============================================================
    function setMeter(bar, pct) {
        if (!bar) return;
        const p = Math.max(0, Math.min(100, pct));
        bar.style.width = p.toFixed(0) + '%';
        bar.parentElement.dataset.level = p >= 90 ? 'danger' : (p >= 70 ? 'warn' : 'ok');
    }
    function fmtDuration(sec) {
        sec = Math.floor(sec);
        const d = Math.floor(sec / 86400);
        const h = Math.floor((sec % 86400) / 3600);
        const m = Math.floor((sec % 3600) / 60);
        if (d > 0) return `${d}天 ${h}小时 ${m}分`;
        if (h > 0) return `${h}小时 ${m}分`;
        return `${m}分`;
    }
    let lastSysInfoError = '';
    function sysInfoUnsupported(text) {
        for (const el of [sysCpu, sysMem, sysProc, sysDisk, sysProcUptime, sysUptime]) {
            if (el) el.textContent = text;
        }
        for (const b of [sysCpuBar, sysMemBar, sysProcBar, sysDiskBar]) {
            if (b) { b.style.width = '0%'; b.parentElement.dataset.level = 'ok'; }
        }
    }
    async function refreshSysInfo() {
        if (document.hidden) return;
        const r = await api('GET', '/api/sysinfo?ts=' + Date.now());
        if (!r.ok || !r.data || !r.data.ok) {
            const reason = r.status === 404
                ? '/api/sysinfo 不存在，请更新后端二进制'
                : '系统信息获取失败：' + (r.error || ('HTTP ' + r.status));
            sysInfoUnsupported(r.status === 404 ? '后端版本不支持' : '获取失败');
            if (reason !== lastSysInfoError) { log(reason); lastSysInfoError = reason; }
            return;
        }
        lastSysInfoError = '';
        const d = r.data;
        // CPU
        if (d.cpu && d.cpu.valid) {
            sysCpu.textContent = `${d.cpu.percent.toFixed(0)}%` +
                (d.cpu.cores ? ` · ${d.cpu.cores} 核` : '') +
                (d.load && d.load.valid ? ` · 负载 ${d.load.l1.toFixed(2)}` : '');
            setMeter(sysCpuBar, d.cpu.percent);
        } else {
            sysCpu.textContent = '采样中…';
            setMeter(sysCpuBar, 0);
        }
        // 系统内存
        if (d.mem && d.mem.valid && d.mem.total_kb > 0) {
            const usedKb = d.mem.total_kb - d.mem.avail_kb;
            const pct = usedKb / d.mem.total_kb * 100;
            sysMem.textContent = `${fmtSize(usedKb*1024)} / ${fmtSize(d.mem.total_kb*1024)} · ${pct.toFixed(0)}%`;
            setMeter(sysMemBar, pct);
        } else { sysMem.textContent = '不可用'; setMeter(sysMemBar, 0); }
        // 进程内存（VmData，对齐看门狗阈值）
        if (d.proc && d.proc.valid) {
            const vmdata = d.proc.vmdata_kb || 0;
            const thr = d.proc.threshold_kb || 0;
            if (thr > 0) {
                const pct = vmdata / thr * 100;
                sysProc.textContent = `${fmtSize(vmdata*1024)} / ${fmtSize(thr*1024)}（重启阈值）`;
                setMeter(sysProcBar, pct);
            } else {
                sysProc.textContent = fmtSize(vmdata*1024);
                setMeter(sysProcBar, 0);
            }
        } else { sysProc.textContent = '不可用'; setMeter(sysProcBar, 0); }
        // 存储
        if (d.disk && d.disk.valid && d.disk.total_bytes > 0) {
            const used = d.disk.total_bytes - d.disk.avail_bytes;
            const pct = used / d.disk.total_bytes * 100;
            sysDisk.textContent = `${fmtSize(used)} / ${fmtSize(d.disk.total_bytes)} · ${pct.toFixed(0)}%`;
            setMeter(sysDiskBar, pct);
        } else { sysDisk.textContent = '不可用'; setMeter(sysDiskBar, 0); }
        // 运行时长：进程（距上次看门狗/手动重启多久）+ 系统（整机开机多久）
        sysProcUptime.textContent = (d.proc_uptime && d.proc_uptime.valid) ? fmtDuration(d.proc_uptime.sec) : '不可用';
        sysUptime.textContent = (d.uptime && d.uptime.valid) ? fmtDuration(d.uptime.sec) : '不可用';
    }
    setInterval(refreshSysInfo, 5000);
    refreshSysInfo();

    // 自动起播
    liveWanted = true;
    syncLiveControls();
    syncLiveVisibility('initial');

    // ============================================================
    // 讲话（浏览器→开发板 单向语音）
    //
    // 浏览器端用 WebCodecs.AudioEncoder（Opus, 48kHz mono）做实时编码，
    // 通过独立的 WebSocket /ws/talk 把 OPUS 帧 binary 发给后端，
    // 后端 talkPlayer 解码 + ALSA 直接播放。
    //
    // ⚠️ 安全上下文限制：
    //   - 浏览器只在 secure context 下才会暴露 navigator.mediaDevices.getUserMedia
    //     与 window.AudioEncoder（WebCodecs）。
    //   - secure context 包括 https://、localhost、127.0.0.1。
    //   - 普通 http://<LAN-IP> 不属于 secure context，整套 API 都是 undefined。
    //   - 因此本按钮只在 location.protocol === 'https:' 或 hostname 是
    //     localhost/127.0.0.1 时显示；其它情况自动隐藏，不影响其它功能。
    //   - 用户首次点击会弹"麦克风"权限请求，浏览器自带 UI（无需我们处理）。
    // ============================================================
    const btnTalk  = document.getElementById('btnTalk');

    function isSecureContextOk() {
        // 受信安全上下文：标准实现优先，兜底用域名判断
        if (typeof window.isSecureContext !== 'undefined') {
            return !!window.isSecureContext;
        }
        if (location.protocol === 'https:') return true;
        const h = location.hostname;
        return (h === 'localhost' || h === '127.0.0.1' || h === '::1');
    }
    function isWebCodecsAudioOk() {
        return typeof window.AudioEncoder === 'function'
            && navigator.mediaDevices && typeof navigator.mediaDevices.getUserMedia === 'function';
    }

    // 仅当安全上下文 + 浏览器支持 WebCodecs Audio 时才显示按钮；
    // 其余情况隐藏（http 访问、Safari 老版本、Firefox < 130 等）。
    if (isSecureContextOk() && isWebCodecsAudioOk()) {
        btnTalk.hidden = false;
    } else {
        btnTalk.hidden = true;
        log('讲话按钮已隐藏：' + (isSecureContextOk()
            ? '当前浏览器不支持 WebCodecs AudioEncoder'
            : '当前为 http 非安全上下文，请改用 https://<ip> 访问'));
    }

    let talkWs = null, talkEnc = null, talkStream = null, talkNode = null, talkCtx = null;
    let talkOn = false;

    function setTalkUi(on, msg) {
        talkOn = on;
        btnTalk.classList.toggle('on', on);
        btnTalk.textContent = on ? '正在讲话' : '讲话';
        if (msg) log(msg);
    }

    async function startTalk() {
        if (talkOn) return;
        try {
            // 1) 申请麦克风（48k mono，禁用浏览器默认的去噪/AGC，避免与 talkPlayer 端码率竞争）
            talkStream = await navigator.mediaDevices.getUserMedia({
                audio: {
                    sampleRate: 48000,
                    channelCount: 1,
                    echoCancellation: true,
                    noiseSuppression: true,
                    autoGainControl: true,
                },
                video: false,
            });

            // 2) 起 WebSocket（与直播分离，独立的 /ws/talk endpoint）
            //    HTTP 模式按钮不显示走不到这；这里强制 wss
            let talkHost = location.hostname;
            if (talkHost.indexOf(':') !== -1 && talkHost[0] !== '[') talkHost = `[${talkHost}]`;
            const wsUrl = `wss://${talkHost}:8444/ws/talk`;
            talkWs = new WebSocket(wsUrl);
            talkWs.binaryType = 'arraybuffer';
            await new Promise((resolve, reject) => {
                const timer = setTimeout(() => reject(new Error('ws connect timeout')), 5000);
                talkWs.onopen = () => {
                    clearTimeout(timer);
                    resolve();
                };
                talkWs.onerror = () => {
                    clearTimeout(timer);
                    reject(new Error('ws error'));
                };
            });
            talkWs.onclose = () => { log('talk ws closed'); stopTalk(); };

            // 3) AudioEncoder（OPUS 48k mono 24kbps）
            talkEnc = new AudioEncoder({
                output: (chunk) => {
                    if (!talkWs || talkWs.readyState !== 1) return;
                    const buf = new ArrayBuffer(chunk.byteLength);
                    chunk.copyTo(buf);
                    talkWs.send(buf);
                },
                error: (e) => log('AudioEncoder error: ' + (e && e.message || e)),
            });
            talkEnc.configure({
                codec: 'opus',
                sampleRate: 48000,
                numberOfChannels: 1,
                bitrate: 24000,
            });

            // 4) AudioContext + AudioWorklet 抽 PCM；为避免引入 worklet 文件，
            //    直接用 ScriptProcessorNode（已 deprecated 但所有主流浏览器仍支持）。
            //    每次 onaudioprocess 给出 bufferSize=2048（48k → ~42.7ms）的 Float32，
            //    构造成 AudioData 喂 encoder。
            talkCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 48000 });
            const src = talkCtx.createMediaStreamSource(talkStream);
            talkNode = talkCtx.createScriptProcessor(2048, 1, 1);
            // ScriptProcessor 必须连到 destination 才会触发回调；用 GainNode(0) 静音返回，避免回声
            const mute = talkCtx.createGain(); mute.gain.value = 0;
            src.connect(talkNode);
            talkNode.connect(mute);
            mute.connect(talkCtx.destination);

            let pcmTsUs = 0; // 微秒
            const frameDurUs = Math.round(2048 * 1e6 / 48000);
            talkNode.onaudioprocess = (ev) => {
                if (!talkEnc || talkEnc.state !== 'configured') return;
                const ch0 = ev.inputBuffer.getChannelData(0);
                // AudioEncoder 期望 AudioData (Float32, planar, f32-planar)
                const ad = new AudioData({
                    format: 'f32-planar',
                    sampleRate: 48000,
                    numberOfFrames: ch0.length,
                    numberOfChannels: 1,
                    timestamp: pcmTsUs,
                    data: ch0,            // f32-planar 单通道直接传
                });
                pcmTsUs += frameDurUs;
                try { talkEnc.encode(ad); } catch (e) { log('encode err:' + e.message); }
                ad.close();
            };

            setTalkUi(true, '🎤 讲话已开始');
        } catch (e) {
            log('startTalk 失败: ' + (e && e.message || e));
            stopTalk();
        }
    }

    function stopTalk() {
        try { if (talkNode)   { talkNode.disconnect(); talkNode.onaudioprocess = null; } } catch(e){}
        try { if (talkCtx)    talkCtx.close(); } catch(e){}
        try { if (talkEnc && talkEnc.state !== 'closed') talkEnc.close(); } catch(e){}
        try { if (talkStream) talkStream.getTracks().forEach(t => t.stop()); } catch(e){}
        try { if (talkWs && talkWs.readyState <= 1) talkWs.close(); } catch(e){}
        talkNode = null; talkCtx = null; talkEnc = null; talkStream = null; talkWs = null;
        setTalkUi(false, '🎤 讲话已结束');
    }

    btnTalk.onclick = () => { if (talkOn) stopTalk(); else startTalk(); };

    // ============================================================
    // 回放 Tab：日期 + 分片列表（B2 将升级为 24h 时间轴）
    // ============================================================
    let pbInited = false;
    const pbDate    = document.getElementById('pbDate');
    const pbRefresh = document.getElementById('pbRefresh');
    const pbCount   = document.getElementById('pbCount');
    const pbVideo   = document.getElementById('pbVideo');
    const pbInfo    = document.getElementById('pbInfo');
    const pbClock   = document.getElementById('pbClock');
    const segList   = document.getElementById('segList');
    const pbPrev    = document.getElementById('pbPrev');
    const pbNext    = document.getElementById('pbNext');
    const pbSpeed   = document.getElementById('pbSpeed');
    const tlCanvas  = document.getElementById('timeline');
    const tlTip     = document.getElementById('tlTip');

    function attemptPlaybackPlay() {
        try {
            const result = pbVideo.play();
            if (result && typeof result.catch === 'function') {
                result.catch(err => {
                    if (err && err.name === 'NotAllowedError') {
                        pbInfo.textContent = '录像已加载，请点击播放器中的播放按钮';
                    } else if (err && err.name !== 'AbortError') {
                        pbInfo.textContent = '播放失败：' + (err.message || err);
                    }
                });
            }
        } catch (e) {
            pbInfo.textContent = '录像已加载，请点击播放器中的播放按钮';
        }
    }

    let pbSegments = [];     // [{name,size,hms,sec, dur}]
    let pbCurIdx   = -1;

    // ---- 回放倍速 ----
    // 原生 <video> 控件菜单只给到 2x，但 playbackRate API 本身支持到 16x。
    // 用自定义下拉框直接设 playbackRate，突破原生上限。
    // 注意：切分片时 pbVideo.src 会被重设，playbackRate 会被浏览器复位成 1.0，
    // 所以把目标倍速存到 pbDesiredRate，并在每次源加载后重新 apply。
    // >2x 自动静音：高倍速音频会刺耳失真，且解码音频徒增开销。
    let pbDesiredRate = 1;
    // 切片重载时刻：teardown / 重设 src 会让浏览器把 playbackRate 复位成 1.0 并触发
    // 一次 ratechange。若此时把 1.0 回写进 pbDesiredRate，就会把用户选的倍速冲掉
    // （表现为"跳转下一段后倍速没了"）。用这个时间戳在重载后的短窗口内屏蔽回写。
    let pbReloadAt = 0;
    function applyPbRate() {
        // 关键：同时设 defaultPlaybackRate。HTML 媒体 load 算法在 src 变更时会把
        // playbackRate 复位成 defaultPlaybackRate（默认 1），所以仅设 playbackRate
        // 在切片重载时会被冲回 1x（加载时序不同，applyPbRate 补救有时来不及）。
        // 把 defaultPlaybackRate 也设成目标倍速，浏览器复位时就直接复位到目标值。
        try { pbVideo.defaultPlaybackRate = pbDesiredRate; } catch(e){}
        try { pbVideo.playbackRate = pbDesiredRate; } catch(e){}
        pbVideo.muted = (pbDesiredRate > 2);
    }
    pbSpeed.onchange = () => {
        pbDesiredRate = parseFloat(pbSpeed.value) || 1;
        applyPbRate();
    };
    // 切片重载后 src 变更会复位 playbackRate，这几个事件兜底重新施加
    pbVideo.addEventListener('loadedmetadata', applyPbRate);
    pbVideo.addEventListener('play',           applyPbRate);
    pbVideo.addEventListener('error', () => {
        const code = pbVideo.error ? pbVideo.error.code : 0;
        const messages = {
            1: '回放加载被中止',
            2: '回放网络错误',
            3: '当前 Safari 无法解码该录像',
            4: '当前 Safari 不支持该录像格式'
        };
        pbInfo.textContent = messages[code] || '回放媒体错误';
    });
    // 用户用原生控件改了倍速时，同步下拉框显示（保持一致）；但重载窗口内的
    // 自动复位（→1.0）不算用户意图，直接忽略，避免冲掉 pbDesiredRate。
    pbVideo.addEventListener('ratechange', () => {
        if (performance.now() - pbReloadAt < 1500) return;
        const r = pbVideo.playbackRate;
        if (Math.abs(r - pbDesiredRate) > 0.01) {
            pbDesiredRate = r;
            // 下拉框若有对应档位就选中，否则不强改
            const opt = Array.from(pbSpeed.options).find(o => parseFloat(o.value) === r);
            if (opt) pbSpeed.value = opt.value;
            pbVideo.muted = (r > 2);
        }
    });

    // ---- 自定义时间叠加层（Q1）----
    //
    // 浏览器原生 <video> 控件显示的是 currentTime / duration（"28:10 / 30:01"），
    // 用户更想看到的是 "现在播到的这一帧对应当时几点几分"。
    //
    // 实现：把"片段起始 epoch（Unix 秒）" 缓存到 pbCurClockBase，每次 timeupdate
    // 取 currentTime 加上去，再格式化为 "YYYY-MM-DD HH:MM:SS"。
    // 监听器一次性绑定，不随 teardownPbMse 解绑——切片时只重置 pbCurClockBase。
    //
    // 注意：segments API 的 s.sec 是"当天 0 点起秒数（设备本地时区）"，文件名也
    // 是设备本地时间命名（YYYYMMDD/HHMMSS.mp4）。浏览器与设备同时区时，用
    // new Date(yyyy, mm-1, dd, 0, 0, 0) 取本地午夜然后加 sec 即可得到墙上时间。
    let pbCurClockBase = 0;   // Unix 秒；0 代表当前没有有效片段，不显示叠加层

    function pbSegEpoch(yyyymmdd, secOfDay) {
        if (!yyyymmdd || yyyymmdd.length !== 8) return 0;
        const y = +yyyymmdd.slice(0, 4);
        const m = +yyyymmdd.slice(4, 6);
        const d = +yyyymmdd.slice(6, 8);
        const t0 = new Date(y, m - 1, d, 0, 0, 0).getTime();
        if (!isFinite(t0)) return 0;
        return Math.floor(t0 / 1000) + ((secOfDay | 0));
    }
    function pbFormatClock(epochMs) {
        const dt = new Date(epochMs);
        const p = (n) => (n < 10 ? '0' : '') + n;
        return `${dt.getFullYear()}-${p(dt.getMonth() + 1)}-${p(dt.getDate())} `
             + `${p(dt.getHours())}:${p(dt.getMinutes())}:${p(dt.getSeconds())}`;
    }
    function updatePbClock() {
        if (!pbCurClockBase) { pbClock.hidden = true; return; }
        const t = pbVideo.currentTime || 0;
        const ms = pbCurClockBase * 1000 + Math.floor(t * 1000);
        pbClock.textContent = pbFormatClock(ms);
        pbClock.hidden = false;
    }
    // 永久绑定：teardownPbMse 只解绑 idx 快路径的 pbTimeListener / pbSeekListener，
    // 这里独立的叠加层监听器跨片段始终生效。
    pbVideo.addEventListener('timeupdate',     updatePbClock);
    pbVideo.addEventListener('seeking',        updatePbClock);
    pbVideo.addEventListener('loadedmetadata', updatePbClock);

    // 小文件预拉：仅缓存一份不超过 8 MiB 的下一片。大录像由 .idx + HTTP Range
    // 按需加载，避免手机为了消除一次切片黑屏而缓存 100 MiB 级完整文件。
    const prefetchCache = new Map();      // url -> { buf: ArrayBuffer, size: number }
    const PREFETCH_LRU_SIZE = 1;
    const PREFETCH_MAX_BYTES = 8 * 1024 * 1024;
    let   prefetchInflight = null;        // { url, ctrl: AbortController }
    let   prefetchTriggered = false;      // 当前片是否已发起过预拉

    async function initPlayback() {
        pbInited = true;
        await loadDays();
        // 首次进入时绘制空时间轴
        drawTimeline();
        window.addEventListener('resize', resizeTimeline);
        resizeTimeline();
    }
    async function loadDays() {
        setChip(pbCount, '加载日期', 'busy');
        const r = await api('GET', '/api/record/days?ts=' + Date.now());
        if (!r.ok) {
            pbDate.innerHTML = '<option>加载失败</option>';
            setChip(pbCount, r.error || '加载失败', 'error');
            return;
        }
        const arr = Array.isArray(r.data) ? r.data : [];
        if (arr.length === 0) {
            pbDate.innerHTML = '<option value="">暂无录像</option>';
            setChip(pbCount, '暂无录像', 'idle');
            pbSegments = []; pbCurIdx = -1; renderSegList(); drawTimeline();
            return;
        }
        pbDate.innerHTML = arr.map(d =>
            `<option value="${escapeHtml(d)}">${escapeHtml(dayPretty(d))}</option>`
        ).join('');
        await loadSegments(pbDate.value);
    }
    async function loadSegments(date) {
        if (!date) {
            pbSegments = []; pbCurIdx = -1; pbCurClockBase = 0;
            setChip(pbCount, '暂无录像', 'idle');
            updatePbClock(); renderSegList(); drawTimeline();
            return;
        }
        setChip(pbCount, '加载录像', 'busy');
        const r = await api('GET', '/api/record/segments?date=' + encodeURIComponent(date) + '&ts=' + Date.now());
        if (!r.ok) {
            setChip(pbCount, r.error || '加载失败', 'error');
            pbSegments = []; renderSegList(); drawTimeline();
            return;
        }
        const arr = Array.isArray(r.data) ? r.data : [];
        // 估算每片时长：用下一片 start - 本片 start；最后一片暂按 600s 兜底
        for (let i = 0; i < arr.length; i++) {
            const a = arr[i], b = arr[i+1];
            if (b) a.dur = Math.max(1, b.sec - a.sec);
            else   a.dur = 600;  // 兜底（生产配置 10min；测试时若 segmentSec=30，仅最后一片显示偏长不影响功能）
        }
        pbSegments = arr;
        setChip(pbCount, `共 ${pbSegments.length} 段`, pbSegments.length ? 'playing' : 'idle');
        pbCurIdx = -1;
        renderSegList();
        drawTimeline();
    }
    function renderSegList() {
        if (pbSegments.length === 0) {
            segList.innerHTML = '<div class="placeholder">该日期没有录像</div>';
            updateNavButtons();
            return;
        }
        const date = pbDate.value;
        // 每行末尾加一个 a[download]：浏览器看到 download 属性会强制走"另存为"，
        // 而不是按 video/mp4 内联播放；title 给鼠标悬停提示文件名。
        // 注意：a 必须与目标资源同源（这里都是当前 origin）才会触发 download，
        // 跨源时浏览器会忽略 download 属性退化为内联打开。
        const html = pbSegments.map((s, i) => {
            const dlUrl  = '/record/' + encodeURIComponent(date) + '/' + encodeURIComponent(s.name);
            // 用日期 + 时间组合一个友好的本地文件名： 20260523_141502.mp4
            const dlName = `${date}_${s.name}`;
            return `
            <div class="it ${i===pbCurIdx?'on':''}" data-idx="${i}">
                <span class="hms">${escapeHtml(s.hms)}</span>
                <span class="nm">${escapeHtml(s.name)}</span>
                <span class="sz">${fmtSize(s.size)}</span>
                <a class="dl" href="${escapeHtml(dlUrl)}" download="${escapeHtml(dlName)}"
                   title="下载 ${escapeHtml(s.name)}">↓</a>
            </div>`;
        }).join('');
        segList.innerHTML = html;
        updateNavButtons();
    }
    function updateNavButtons() {
        pbPrev.disabled = !(pbCurIdx > 0);
        pbNext.disabled = !(pbCurIdx >= 0 && pbCurIdx < pbSegments.length - 1);
    }
    // 当前正在用 MSE 播的片：用于"同片内 seek"的就地跳转
    //   pbCurUrl —— 当前 video.src 对应的 mp4 url（非 blob，是逻辑标识）
    //   pbCurSb / pbCurMs —— 当前 SourceBuffer / MediaSource，留给 seek 时判断 buffered
    let pbCurUrl = null;

    // 判断 buffered 是否已经覆盖到指定时刻（带 0.2s 余量，与 tryDoSeek 一致）
    function pbBufferedCovers(t) {
        try {
            const b = pbVideo.buffered;
            for (let i = 0; b && i < b.length; i++) {
                if (t + 0.2 >= b.start(i) && t + 0.2 < b.end(i)) return true;
            }
        } catch(e) {}
        return false;
    }

    function playSeg(idx, seekOffset) {
        if (idx < 0 || idx >= pbSegments.length) return;
        const s = pbSegments[idx];
        const date = pbDate.value;
        const url = '/record/' + encodeURIComponent(date) + '/' + encodeURIComponent(s.name);
        const want = (typeof seekOffset === 'number' && seekOffset > 0) ? seekOffset : 0;

        // 叠加层：每次进 playSeg 都重设当前片的 epoch base，timeupdate 即取
        // currentTime 拼出墙上时间。同片就地跳转的快路径下面也要走到这一行
        // 才能 update（不会发 timeupdate）——所以放在分支前。
        pbCurClockBase = pbSegEpoch(date, s.sec | 0);
        updatePbClock();

        // ---- 同片就地跳转（避免 video.src 重置导致 player 控件"先缩小再撑大"）----
        //
        // playSeg 之前任何情况下都会走 playSegByMse → teardownPbMse → new MediaSource
        // → pbVideo.src = URL.createObjectURL(ms)，浏览器把 video 当全新源处理：
        // 视频面板会先塌成默认尺寸（loadstart 阶段），再等首帧撑回来，视觉上就是
        // 用户看到的"窗口变小再变大、不连贯"。
        //
        // 实际上：
        //   1) idx 没变 → 同一个 mp4，currentTime 已经在播，buffered 里如果已包含
        //      目标时刻（比如点了"5 秒前/后"或预拉早就把后段填进去了），直接
        //      pbVideo.currentTime = want 就能瞬间跳到，没有任何视觉抖动。
        //   2) idx 没变但 buffered 没覆盖 → 仍要触发新一轮 idx + Range 拉取，
        //      但这种情况后续再处理（同片内追加 append 还需要把 SourceBuffer
        //      reset 否则 MSE 会觉得时序跳跃；这里走原路径 fallback 不会变差）。
        if (idx === pbCurIdx && pbCurUrl === url && pbBufferedCovers(want)) {
            try {
                if (Math.abs((pbVideo.currentTime || 0) - want) > 0.05) {
                    pbVideo.currentTime = want;
                }
            } catch(e) {}
            attemptPlaybackPlay();
            pbInfo.textContent = `▶ ${dayPretty(date)} ${s.hms} · ${s.name} · ${fmtSize(s.size)}`;
            updateNavButtons();
            drawTimeline();
            return;
        }

        pbCurIdx = idx;
        pbCurUrl = url;
        prefetchTriggered = false;
        // 切片时取消正在进行的预拉（如果在拉的不是即将播的这片）
        if (prefetchInflight && prefetchInflight.url) {
            if (prefetchInflight.url !== url) {
                try { prefetchInflight.ctrl.abort(); } catch(e){}
                prefetchInflight = null;
            }
        }

        // 关键：fmp4（empty_moov+frag_keyframe）直接 video.src=url 时
        //   浏览器无法计算 duration（mvhd duration=0），导致进度条不显示、拖动失效。
        //   走 MSE：fetch 完整字节 → appendBuffer → endOfStream()，
        //   MSE 内部根据所有 moof 的 sample duration 之和算出 duration，进度条才会正常。
        pbInfo.textContent = `▶ ${dayPretty(date)} ${s.hms} · ${s.name} · ${fmtSize(s.size)}`;
        playSegByMse(url, s, seekOffset).catch(e => {
            pbInfo.textContent = '回放初始化失败：' + (e && e.message || e);
        });
        for (const el of segList.querySelectorAll('.it')) {
            el.classList.toggle('on', Number(el.getAttribute('data-idx')) === idx);
        }
        const cur = segList.querySelector('.it.on');
        if (cur) cur.scrollIntoView({ block:'nearest' });
        updateNavButtons();
        drawTimeline();
    }

    // MSE 加载单个 fmp4 片段
    //
    // 设计要点：
    //   1) **流式 append**（首字节即开播）—— 用 fetch().body.getReader() 边读边
    //      pbSb.appendBuffer，不再 await arrayBuffer() 等整片下完。fmp4
    //      （ftyp + moov + frag_keyframe）只需前几百 KB 即可解出首帧，10 分钟 60MB
    //      的片子从"5~10s 等"降到"几百 ms 出图"。
    //   2) **epoch 防护** —— 来回点切片时旧的 async 流程仍在 await 某个 chunk，
    //      唤醒后会向**新片**的 pbSb 塞**旧片**的字节，触发
    //      INVALID_STATE_ERR / QuotaExceeded，video 进 error 直至崩页。
    //      用 pbEpoch 自增计数：每次 playSegByMse 进入时拿到 myEpoch；任何 await
    //      返回点立刻校验 myEpoch 是否还等于全局 pbEpoch，不等就直接早退。
    //   3) **主动 abort** —— teardown 时 abort 旧 fetch，让对端 fd 早关，避免
    //      服务端 WriteAllBlocking 等 5s 才感知断连，减轻 detached thread 堆积压力。
    let pbMs = null, pbSb = null, pbAbort = null, pbAttachment = null;
    let pbEpoch = 0;
    // ---- "按需 lookahead"补片所需的额外状态（只在走过 idx 快路径时填充）----
    //   pbCurIdxJson    —— 当前片的 .idx 解析结果（fragment 表）
    //   pbAppendedFrags —— Set<int>：已成功 append 进 SourceBuffer 的 fragment 索引集合
    //                     （注意是 Set 而不是连续区间，因为按需 lookahead 后会形成多段非连续区间，
    //                      例如 [0..7] 和 [200..207]）
    //   pbCurUrlForSeek —— 与 pbCurIdxJson 配套的录像 url（防止跨片串改）
    //   pbSeekFetch     —— 当前正在进行的"按需补 fragment"请求（拖动 / 续播触发）
    //   pbSeekListener / pbTimeListener —— 注册到 video 的回调，teardown 时解绑
    let pbCurIdxJson    = null;
    let pbCurUrlForSeek = null;
    let pbAppendedFrags = null;
    let pbSeekFetch     = null;
    let pbSeekListener  = null;
    let pbTimeListener  = null;
    // ensureLookahead 是 playSegByMse 内部闭包（捕获了 myEpoch / refillFrom），
    // 但 waiting 事件 / 500ms 兜底轮询在外层，需要能调它来"卡住时继续补最后的片"。
    // 故把当前片的 ensureLookahead 暴露到这个模块级引用，teardown 时清空。
    let pbEnsureLookahead = null;
    // 真理之源：本片字节是否已全部 append 进 SourceBuffer。
    // 只有这个为 true 时，"currentTime 到达 buffered 末端"才能解释为"播完"。
    // 否则 stall 在末端只能解释为"lookahead/seek refill 还在路上"，绝不能切片。
    // 三条路径（idx / 缓存 / 流式）各自在自己的"完成"时刻把它置 true。
    let pbAllAppended = false;
    function teardownPbMse() {
        try { if (pbAbort) pbAbort.abort(); } catch(e){}
        pbAbort = null;
        try { if (pbSeekFetch) pbSeekFetch.ctrl.abort(); } catch(e){}
        pbSeekFetch = null;
        if (pbSeekListener) {
            try { pbVideo.removeEventListener('seeking', pbSeekListener); } catch(e){}
            pbSeekListener = null;
        }
        if (pbTimeListener) {
            try { pbVideo.removeEventListener('timeupdate', pbTimeListener); } catch(e){}
            pbTimeListener = null;
        }
        pbEnsureLookahead = null;
        pbCurIdxJson    = null;
        pbCurUrlForSeek = null;
        pbAppendedFrags = null;
        pbAllAppended   = false;
        try { if (pbMs && pbMs.readyState === 'open') pbMs.endOfStream(); } catch(e){}
        if (pbAttachment) {
            detachMediaSource(pbVideo, pbAttachment);
            pbAttachment = null;
            try { pbVideo.load(); } catch(e){}
        }
        pbMs = null; pbSb = null;
    }
    // 等 SourceBuffer 当前 append 完成；若 epoch 已过期返回 false，调用方应早退
    function waitForUpdateEnd(sb, myEpoch) {
        return new Promise((res) => {
            const onEnd = () => { sb.removeEventListener('error', onErr); res(true); };
            const onErr = () => { sb.removeEventListener('updateend', onEnd); res(false); };
            sb.addEventListener('updateend', onEnd, { once:true });
            sb.addEventListener('error',     onErr, { once:true });
        }).then(ok => ok && (myEpoch === pbEpoch));
    }
    // 把一段字节安全 append 到 sb；成功 true，失败/被取消 false
    async function safeAppend(sb, chunk, myEpoch) {
        if (myEpoch !== pbEpoch) return false;
        try { sb.appendBuffer(chunk); }
        catch (e) { console.warn('appendBuffer throw', e); return false; }
        return await waitForUpdateEnd(sb, myEpoch);
    }

    // 取伴生 .idx 索引文件并解析。
    //
    // 服务端格式（recorder.cpp::AppendIdxEntry_locked）：
    //   {"v":1,"ts":90000,"init":<bytes>,"frags":[[<tfdt90k>,<offset>,<size>], ...]}
    //
    // 容错：
    //   - 404 → 返回 null（旧录像无伴生 idx，回退到全量流式）
    //   - 半成品 .idx（正在录的最新片：缺尾部 "]}"）→ 自己补全再 parse
    //   - 任何异常都返回 null 走兜底分支
    async function fetchSegIndex(url, signal) {
        try {
            const r = await fetch(url + '.idx',
                { signal, cache: 'no-store' });
            if (!r.ok) return null;
            let txt = await r.text();
            txt = txt.trim();
            if (!txt.startsWith('{')) return null;
            let idx = null;
            try {
                idx = JSON.parse(txt);
            } catch (e) {
                // 半成品：尝试补全 "]}"
                let fixed = txt;
                // 去掉末尾可能的逗号或不完整片段
                // 找最后一个 "]"，截断到那里再补 "]}"
                const lastBracket = fixed.lastIndexOf(']');
                if (lastBracket > 0) {
                    fixed = fixed.substring(0, lastBracket + 1) + ']}';
                    try { idx = JSON.parse(fixed); }
                    catch (e2) { return null; }
                } else { return null; }
            }
            if (!idx || idx.v !== 1 || !Array.isArray(idx.frags)) return null;
            return idx;
        } catch (e) {
            return null;
        }
    }

    // 在 fragment 数组里找：tfdt <= wantSeek*ts 的最大那条（即 wantSeek 落在的 fragment）
    function pickFragForSeek(idx, wantSeek) {
        const ts = idx.ts || 90000;
        const target = wantSeek * ts;
        let lo = 0, hi = idx.frags.length - 1, ans = 0;
        while (lo <= hi) {
            const m = (lo + hi) >> 1;
            if (idx.frags[m][0] <= target) { ans = m; lo = m + 1; }
            else hi = m - 1;
        }
        return ans;
    }
    async function playSegByMse(url, seg, seekOffset) {
        const myEpoch = ++pbEpoch;     // 占用本次 epoch
        pbReloadAt = performance.now(); // 标记重载窗口，屏蔽复位引发的 ratechange 回写
        teardownPbMse();
        if (!supportsMediaType(MediaSourceCtor, MIME)) {
            // iOS 17.0 及更早版本没有 MSE/MMS，回退到 Safari 原生 fMP4。
            // 原生路径能播放和按 HTTP Range 续载，但 empty_moov 录像的总时长/精准拖动
            // 可能不如 .idx + MSE 路径完整。
            pbVideo.src = url;
            if (typeof seekOffset === 'number' && seekOffset > 0) {
                pbVideo.addEventListener('loadedmetadata',
                    () => { try { pbVideo.currentTime = seekOffset; } catch(e){} },
                    { once: true });
            }
            pbInfo.textContent = isIosDevice
                ? '当前 iOS 使用原生 MP4 回放；如无法拖动请升级至 iOS 17.1+'
                : '当前浏览器使用原生 MP4 回放';
            attemptPlaybackPlay();
            return;
        }
        let attachment;
        try {
            attachment = attachMediaSource(pbVideo, MediaSourceCtor);
        } catch (e) {
            pbInfo.textContent = '媒体源初始化失败，已切换原生回放';
            pbVideo.src = url;
            attemptPlaybackPlay();
            return;
        }
        const ms = attachment.mediaSource;
        pbAttachment = attachment;
        pbMs = ms;

        // ---------- seek 跟踪状态（在整个函数生命周期内由 append 循环驱动）----------
        // 之前用浏览器 progress 事件来触发 seek 是错的：MSE 模式下 <video> 的 progress
        //   事件不依赖 SourceBuffer.append，触发时机不可控（有时根本不发）。
        //   现在改成"我们自己每次 append 完一个 chunk 就主动判断 buffered 是否覆盖到目标"。
        const wantSeek = (typeof seekOffset === 'number' && seekOffset > 0)
                         ? seekOffset : 0;
        // 即使从 0 秒开始也要等首批 append 后确认实际可解码起点。旧录像可能在
        // P 帧处切 fragment，WebKit 会接受数据但把 buffered.start() 推迟到下一个
        // IDR；若播放头仍留在 0，readyState 会一直停在 HAVE_METADATA。
        let seekDone = false;
        // 调用：每次 append 完 chunk 后调一次。命中目标即 seek 并标记完成。
        function tryDoSeek() {
            if (seekDone) return;
            if (myEpoch !== pbEpoch) { seekDone = true; return; }
            try {
                const b = pbVideo.buffered;
                for (let i = 0; b && i < b.length; i++) {
                    const start = b.start(i);
                    const end = b.end(i);
                    // 给 0.2s 余量：buffered.end 刚好等于 wantSeek 时浏览器有时拒绝 seek
                    if (wantSeek < end && (wantSeek + 0.2 >= start || wantSeek < start)) {
                        // WebKit 对时间洞较严格，落点稍微越过 range 起点更稳定。
                        const target = Math.max(wantSeek, start + 0.001);
                        pbVideo.currentTime = Math.min(target, end - 0.001);
                        attemptPlaybackPlay();
                        seekDone = true;
                        return;
                    }
                }
            } catch (e) { /* 下次 chunk 再试 */ }
        }

        // ★ 拿到第一帧（loadeddata）就立刻 play()，让画面尽快出来。
        //   但若有 seek 需求且尚未完成，先不要从 0 播 —— 否则会出现"从片头播一会再跳"的
        //   尴尬体验。这种情况下 play 由 tryDoSeek() 触发。
        const onFirstFrame = () => {
            if (myEpoch !== pbEpoch) return;
            if (!seekDone) return;     // 等 seek 完成后再 play
            attemptPlaybackPlay();
        };
        pbVideo.addEventListener('loadeddata', onFirstFrame, { once: true });

        // 等 MediaSource 打开
        const opened = await waitForMediaSourceOpen(ms);
        if (myEpoch !== pbEpoch) return;
        if (!opened) {
            teardownPbMse();
            pbInfo.textContent = '媒体源打开超时，已切换原生回放';
            pbVideo.src = url;
            attemptPlaybackPlay();
            return;
        }
        if (pbAttachment && pbAttachment.objectUrl) {
            try { URL.revokeObjectURL(pbAttachment.objectUrl); } catch(e) {}
            pbAttachment.objectUrl = '';
        }

        let sb;
        try {
            sb = ms.addSourceBuffer(MIME);
            sb.mode = 'segments';
        } catch (e) {
            detachMediaSource(pbVideo, pbAttachment);
            pbAttachment = null;
            pbVideo.src = url;
            attemptPlaybackPlay();
            return;
        }
        pbSb = sb;

        // ---------- 精准跳转 / 即播 统一走 .idx 快速路径（HTTP Range + 伴生 .idx 索引）----------
        //
        // 入口策略：
        //   - 命中预拉缓存（cachedForRange != null）→ 不走 idx，直接内存 append（更快）
        //   - 其它一切情况（含点文件名 wantSeek=0）→ 优先走 idx；解析失败再回退到全量流式
        //
        // 改进的 idx 路径核心思路："不一口气拉到 EOF，按需 lookahead"：
        //   1) GET .idx
        //   2) Range bytes=0-init-1 拉 init segment
        //   3) 立刻按 idx 估算总时长写到 mediaSource.duration → slider 出总时长可拖动
        //   4) 当前播放位置所在 fragment 起，**只 append N 个 fragment 的 lookahead**
        //      （N 由 LOOKAHEAD_SEC 控制，约 8s 视频）；append 完即 SourceBuffer 空闲
        //   5) timeupdate 监听：当 buffered.end - currentTime < LOOKAHEAD_LOW_SEC 时
        //      继续追加下一批 lookahead；按需即可，永不阻塞 SourceBuffer
        //   6) seeking 监听：拖动时立即拉一批以目标 fragment 为起点的 lookahead
        //
        // 这样修复了两个问题：
        //   - 直接点文件名（wantSeek=0）也能立刻显示总时长（之前条件是 wantSeek > 0）
        //   - 拖动响应慢（之前 SourceBuffer 被 30~60s 的"流式到 EOF"占住，
        //     ensureBufferedAt 的 Range 字节回来后要排队 safeAppend；现在初始 batch
        //     ~1s 就完成 append，SourceBuffer 立刻空闲，拖动 Range 几乎瞬时落地）
        const LOOKAHEAD_SEC      = 8;   // 每批预拉 ~8s 视频
        const LOOKAHEAD_LOW_SEC  = 4;   // buffered 余量 < 4s 时触发下一批

        // 公共工具：从 fragStart 起选若干个连续 fragment（累计时长 ≥ wantSec）拉一段字节
        // append 进 sb；返回 true 表示成功 append 了至少一个 fragment。
        // 已 append 过的 fragment 会被跳过（避免重复字节）。
        async function fetchAndAppendBatch(fragStart, wantSec, myEpoch, signal) {
            if (myEpoch !== pbEpoch) return false;
            if (!pbCurIdxJson || !pbSb) return false;
            const jx = pbCurIdxJson;
            const ts = jx.ts || 90000;
            // 跳过已 append 的 fragment
            while (fragStart < jx.frags.length &&
                   pbAppendedFrags && pbAppendedFrags.has(fragStart)) {
                fragStart++;
            }
            if (fragStart >= jx.frags.length) return false;
            // 选连续区间 [fragStart, fragEnd]：累计时长 >= wantSec 或遇到已 append 边界
            let fragEnd = fragStart;
            const tStart = jx.frags[fragStart][0];
            while (fragEnd + 1 < jx.frags.length) {
                if (pbAppendedFrags && pbAppendedFrags.has(fragEnd + 1)) break;
                const accSec = (jx.frags[fragEnd + 1][0] - tStart) / ts;
                if (accSec >= wantSec) break;
                fragEnd++;
            }
            const a = jx.frags[fragStart];      // [tfdt, off, sz]
            const b = jx.frags[fragEnd];
            const rangeStart = a[1];
            const rangeEnd   = b[1] + b[2] - 1;
            const t0 = performance.now();
            try {
                const r = await fetch(pbCurUrlForSeek, {
                    signal, cache: 'no-store',
                    headers: { 'Range': `bytes=${rangeStart}-${rangeEnd}` }
                });
                if (myEpoch !== pbEpoch) return false;
                if (!r.ok && r.status !== 206) return false;
                const ab = await r.arrayBuffer();
                if (myEpoch !== pbEpoch) return false;
                const ok = await safeAppend(pbSb, new Uint8Array(ab), myEpoch);
                if (!ok) return false;
                for (let k = fragStart; k <= fragEnd; k++) pbAppendedFrags.add(k);
                console.log(`[idx] batch frag #${fragStart}..${fragEnd}/${jx.frags.length} ` +
                            `(${fmtSize(ab.byteLength)}) in ${(performance.now()-t0).toFixed(0)}ms`);
                // 全部 fragment 都已 append 时 signal endOfStream，让 <video> 在
                // currentTime 抵达 duration 时正常触发 'ended' 事件（自动切下一片）。
                // 不调 endOfStream 时 MediaSource 始终停在 'open'，浏览器认为还会有
                // 更多数据，不发 'ended'，自动续播链路就断了。
                // 此时 buffered.end(last) ≈ 我们估算的 ms.duration，endOfStream 重置
                // duration 影响可忽略；且因为全片在手，回拖也不再补片，不会再抖动。
                if (pbAppendedFrags.size === jx.frags.length &&
                    pbMs && pbMs.readyState === 'open') {
                    pbAllAppended = true;       // ← 真理之源：整片在手
                    const eos = () => {
                        if (myEpoch !== pbEpoch) return;
                        try {
                            if (pbMs && pbMs.readyState === 'open') {
                                pbMs.endOfStream();
                                console.log('[idx] all frags appended → endOfStream()');
                            }
                        } catch (e) { console.warn('[idx] endOfStream failed', e); }
                    };
                    if (!pbSb.updating) eos();
                    else pbSb.addEventListener('updateend', eos, { once: true });
                }
                return true;
            } catch (e) {
                if (e && e.name !== 'AbortError') {
                    console.warn('[idx] batch fetch failed', e);
                }
                return false;
            }
        }

        let usedIdxFastPath = false;
        const cachedForRange = prefetchCache.get(url);
        if (!cachedForRange) {
            const probeAbort = new AbortController();
            // 复用 pbAbort 以便切片时一并 abort
            pbAbort = probeAbort;
            const idx = await fetchSegIndex(url, probeAbort.signal);
            if (myEpoch !== pbEpoch) return;
            if (idx && idx.frags.length > 0 && idx.init > 0) {
                const ts0 = idx.ts || 90000;
                // wantSeek=0 时 fi=0；wantSeek>0 时 fi=目标 fragment
                const fi  = wantSeek > 0 ? pickFragForSeek(idx, wantSeek) : 0;
                console.log(`[idx] hit: frag #${fi}/${idx.frags.length} ` +
                            `(want=${wantSeek}s, totalDur≈${(idx.frags[idx.frags.length-1][0]/ts0).toFixed(0)}s)`);
                pbInfo.textContent = wantSeek > 0
                    ? `⚡ 精准跳转（idx #${fi+1}/${idx.frags.length}）`
                    : `⚡ 即播（idx #1/${idx.frags.length}）`;
                // 把 idx 暴露给 seeking / timeupdate 监听器，让它能"按需补片"
                pbCurIdxJson    = idx;
                pbCurUrlForSeek = url;
                pbAppendedFrags = new Set();

                // 1) 先取 init segment（bytes=0-<init-1>）
                let initOk = false;
                try {
                    const r1 = await fetch(url, {
                        signal: probeAbort.signal,
                        cache:  'no-store',
                        headers: { 'Range': `bytes=0-${idx.init - 1}` }
                    });
                    if (myEpoch !== pbEpoch) return;
                    if (r1.ok || r1.status === 206) {
                        const ab = await r1.arrayBuffer();
                        if (myEpoch !== pbEpoch) return;
                        initOk = await safeAppend(sb, new Uint8Array(ab), myEpoch);
                    }
                } catch (e) { /* 静默回退 */ }

                // 2) 立刻锁定 mediaSource.duration（按 .idx 估算总时长）
                // 背景：原生 <video> 控件只有在 duration 是有限正数时才会渲染总时长 +
                //   启用进度条拖动；走 endOfStream 自动算 duration 要等流读完，
                //   30min/80MB / 1MB/s 上行 = 30~60s 的真空期，整段 slider 不可拖。
                // 解：.idx 里已经有所有 fragment 的 tfdt（90kHz），用
                //   tfdt(last)/ts + 末段时长 估算总时长，立即写到 ms.duration。
                //   MSE 规范允许任意时机 write（sb 不在 updating 即可），且后续
                //   appendBuffer 即使超了估算 duration，MSE 也会自动扩展。
                if (initOk && idx.frags.length > 0) {
                    const ts   = ts0;
                    const last = idx.frags.length - 1;
                    let lastFragSec = (last > 0)
                        ? (idx.frags[last][0] - idx.frags[last - 1][0]) / ts
                        : 0;
                    if (!isFinite(lastFragSec) || lastFragSec <= 0) lastFragSec = 0;
                    // +1.0s 余量；估小了 MSE 自动扩，估大了 video 自身按 sample 时长走
                    const estDur = idx.frags[last][0] / ts + Math.max(lastFragSec, 0.5) + 1.0;
                    try {
                        if (!pbSb.updating && isFinite(estDur) && estDur > 0) {
                            pbMs.duration = estDur;
                            console.log(`[idx] ms.duration set = ${estDur.toFixed(2)}s ` +
                                        `(${idx.frags.length} frags)`);
                        }
                    } catch (e) {
                        console.warn('[idx] set ms.duration failed', e);
                    }
                }

                // 3) 拉初始 lookahead 批次（fi 起，约 LOOKAHEAD_SEC 视频）
                if (initOk) {
                    const ok = await fetchAndAppendBatch(fi, LOOKAHEAD_SEC, myEpoch,
                                                         probeAbort.signal);
                    if (myEpoch !== pbEpoch) return;
                    if (ok) {
                        usedIdxFastPath = true;
                        tryDoSeek();
                        attemptPlaybackPlay();
                    }
                }
            }
            if (myEpoch !== pbEpoch) return;
        }

        if (usedIdxFastPath) {
            // 已经走过 idx 快路径，跳过下面的"命中缓存 / 全量流式"分支
            // 注意：这里**不调 ms.endOfStream()**。原因：
            //   1) 我们已经在 init append 完后显式设了 ms.duration（按 .idx 估算总时长），
            //      浏览器原生 slider 已经能正常显示与拖动；
            //   2) endOfStream() 会把 duration 重置成 buffered.end(last)，覆盖我们设的值，
            //      还会让 ms 进入 ended 状态——后续向前补片 appendBuffer 时虽然 MSE 会自动
            //      切回 open，但来回切换会频繁触发 durationchange，体验抖动；
            //   3) 留 ms 在 open 状态对功能没影响：video 仍能播到最后一帧，只是没有"流结束"
            //      事件；ended 由 video 自身在 currentTime 到 duration 时正常触发。
            tryDoSeek();
            attemptPlaybackPlay();

            // ---------- 注册 seeking + timeupdate 监听：让进度条拖动 / 续播都能按需补片 ----------
            // 背景：
            //   - 初始 batch 只 append 了 [fi, fi+lookahead] 几个 fragment；其它位置都不在
            //     SourceBuffer.buffered 内。
            //   - <video> 控件原生进度条直接改 currentTime，不会经过 playSeg；
            //     正常播放也只是流逝 currentTime。两者都需要我们主动补字节。
            // seeking 处理：拖动到 t（无论前后），找到目标 fragment fi'，从 fi' 起拉一批
            //   LOOKAHEAD_SEC 视频；mode='segments' 下新 fragment 会按 tfdt 自动归位，
            //   不重建 SourceBuffer / <video>，无视口闪烁。
            // timeupdate 处理：每次播放进度更新，看 buffered.end-currentTime 是否 < LOOKAHEAD_LOW_SEC，
            //   小于则提前续传下一批；保证连续播放永不 waiting。
            // 容错：
            //   - 多次事件（拖动会狂触发）：用 pbSeekFetch 单飞 + 起点终点 dedup
            //   - 切片 / teardown：teardownPbMse 会 abort pbSeekFetch 并解绑两个 listener
            //   - epoch 防护：闭包捕获 myEpoch，被切走时立即早退
            //   - MS 始终 open：idx 快路径不再调 endOfStream，appendBuffer 可直接生效
            const refillFrom = async (fragStart, wantSec, after) => {
                if (myEpoch !== pbEpoch) return;
                if (!pbCurIdxJson) return;
                if (fragStart < 0 || fragStart >= pbCurIdxJson.frags.length) return;
                // dedup：同一 fragStart 已经在拉就不重复发
                if (pbSeekFetch && pbSeekFetch.fragStart === fragStart) return;
                if (pbSeekFetch) {
                    try { pbSeekFetch.ctrl.abort(); } catch(e){}
                    pbSeekFetch = null;
                }
                const ctrl = new AbortController();
                pbSeekFetch = { ctrl, fragStart };
                try {
                    const ok = await fetchAndAppendBatch(fragStart, wantSec, myEpoch, ctrl.signal);
                    if (myEpoch !== pbEpoch) return;
                    if (ok && typeof after === 'function') after();
                } finally {
                    if (pbSeekFetch && pbSeekFetch.ctrl === ctrl) pbSeekFetch = null;
                }
            };
            const ensureBufferedAt = async (t) => {
                if (myEpoch !== pbEpoch) return;
                if (!pbCurIdxJson || !pbSb || !pbMs) return;
                if (pbBufferedCovers(t)) return;
                const fi = pickFragForSeek(pbCurIdxJson, t);
                console.log(`[idx] seek refill: t=${t.toFixed(2)}s frag=${fi}`);
                await refillFrom(fi, LOOKAHEAD_SEC, () => {
                    // 字节到位后再触发一次 seek，让 video 真的跳过去
                    try { pbVideo.currentTime = t; } catch(e){}
                    attemptPlaybackPlay();
                });
            };
            // ensureLookahead：决定何时补下一批 fragment。
            //
            // 之前两版都有 bug：
            //   v1: 用 buffered.end - currentTime 做判断 → 末段 cur 越过 bEnd 死锁
            //   v2: "找第一个全局未 append 的 fragment" → 用户拖到尾部后，
            //       lookahead 一直去 backfill 最小 index 缺口，**播放头前面真正
            //       需要的 fragment 永远拉不到**（典型现象：日志里 cur 卡在 1792.1
            //       不动，但 lookahead 在拉 frag 50..114 这种离 cur 1700s 远的位置）
            //
            // 这版正确策略：**前向优先 + 后向 backfill**
            //   1) 从 currentTime 所在 fragment 起向后找第一个未 append（forwardNext）
            //      → 这是播放头马上要用的字节，优先级最高
            //   2) forwardNext 距 currentTime <= LOOKAHEAD_LOW_SEC 才触发拉
            //      （>LOOKAHEAD_LOW_SEC 说明前向 buffered 充足，先不打扰）
            //   3) forwardNext 全已 append（前向完整）→ 才考虑 backfill 之前的缺口，
            //      让 pbAppendedFrags 最终能涨到 N，pbAllAppended 转 true
            //   4) backfill 不受 LOOKAHEAD_LOW_SEC 约束（因为不是给当前播放用的）
            const ensureLookahead = () => {
                if (myEpoch !== pbEpoch) return;
                if (!pbCurIdxJson || !pbSb) return;
                if (pbAllAppended) return;        // 全片已在手，无需再拉
                if (pbSeekFetch) return;          // 已经在拉，等它完成

                const N  = pbCurIdxJson.frags.length;
                const ts = pbCurIdxJson.ts || 90000;
                const t  = pbVideo.currentTime || 0;

                // 步骤 1：前向优先 —— cur 所在 fragment 起找第一个未 append
                const curFrag = pickFragForSeek(pbCurIdxJson, t);
                let forwardNext = -1;
                for (let i = curFrag; i < N; i++) {
                    if (!pbAppendedFrags.has(i)) { forwardNext = i; break; }
                }

                if (forwardNext >= 0) {
                    // 有前向缺口：判断 lookahead 触发距离
                    const fragStart = pbCurIdxJson.frags[forwardNext][0] / ts;
                    if (fragStart - t > LOOKAHEAD_LOW_SEC) return;
                    refillFrom(forwardNext, LOOKAHEAD_SEC);
                    return;
                }

                // 步骤 2：前向完整 → backfill 后向缺口（不受 LOOKAHEAD_LOW_SEC 约束）
                for (let i = 0; i < curFrag; i++) {
                    if (!pbAppendedFrags.has(i)) {
                        refillFrom(i, LOOKAHEAD_SEC);
                        return;
                    }
                }
                // 这里理论不会到（前向后向都没缺口 = 全 append，pbAllAppended 应已为 true）
            };
            pbSeekListener = () => { ensureBufferedAt(pbVideo.currentTime || 0); };
            pbTimeListener = () => { ensureLookahead(); };
            pbVideo.addEventListener('seeking',    pbSeekListener);
            pbVideo.addEventListener('timeupdate', pbTimeListener);
            // 暴露给外层：waiting 事件 / 500ms 兜底轮询在播放头 stall（timeupdate 停发）
            // 时也能继续补最后的 fragment，避免"卡在末尾→N-1 永远拉不到→不切片"死锁。
            pbEnsureLookahead = ensureLookahead;

            const fixDur2 = () => {
                if (myEpoch !== pbEpoch) return;
                if (isFinite(pbVideo.duration) && pbVideo.duration > 0) {
                    seg.dur = Math.max(1, Math.round(pbVideo.duration));
                    drawTimeline();
                }
            };
            pbVideo.addEventListener('durationchange', fixDur2, { once:true });
            pbVideo.addEventListener('loadedmetadata', fixDur2, { once:true });
            return;
        }

        // ---------- 命中预拉缓存：内存里直接分块 append ----------
        const cached = prefetchCache.get(url);
        if (cached) {
            const buf = new Uint8Array(cached.buf);
            // LRU 触摸
            prefetchCache.delete(url); prefetchCache.set(url, cached);
            pbInfo.textContent = '⚡ 命中预拉缓存（' + fmtSize(cached.size) + '）';
            const chunkSize = 4 * 1024 * 1024;
            let off = 0;
            while (off < buf.length) {
                if (myEpoch !== pbEpoch) return;
                const end = Math.min(off + chunkSize, buf.length);
                const ok = await safeAppend(sb, buf.subarray(off, end), myEpoch);
                if (!ok) return;
                off = end;
                tryDoSeek();   // ← 每次 append 完就尝试 seek
            }
        } else {
            // ---------- 流式 fetch + ReadableStream.getReader() ----------
            pbAbort = new AbortController();
            const myAbort = pbAbort;
            let resp;
            try {
                resp = await fetch(url, { signal: myAbort.signal, cache: 'no-store' });
            } catch (e) {
                return; // 通常是 AbortError（被新片切走），静默
            }
            if (myEpoch !== pbEpoch) return;
            if (!resp.ok || !resp.body || !resp.body.getReader) {
                // 无 ReadableStream 的旧浏览器直接交给原生 video，避免把完整录像
                // arrayBuffer 读进手机内存。此路径可能缺少精准时长，但能稳定播放。
                if (!resp.ok) return;
                teardownPbMse();
                pbVideo.src = url;
                if (wantSeek > 0) {
                    pbVideo.addEventListener('loadedmetadata', () => {
                        try { pbVideo.currentTime = wantSeek; } catch(e) {}
                    }, { once: true });
                }
                attemptPlaybackPlay();
                return;
            } else {
                const reader = resp.body.getReader();
                let totalBytes = 0;
                let firstChunkLogged = false;
                const t0 = performance.now();
                while (true) {
                    let r;
                    try { r = await reader.read(); }
                    catch (e) { return; }       // 被 abort
                    if (myEpoch !== pbEpoch) {
                        try { reader.cancel(); } catch(e){}
                        return;
                    }
                    if (r.done) break;
                    const ok = await safeAppend(sb, r.value, myEpoch);
                    if (!ok) {
                        try { reader.cancel(); } catch(e){}
                        return;
                    }
                    totalBytes += r.value.byteLength;
                    tryDoSeek();   // ← 每个 chunk append 完都试一下，命中即 seek+play
                    if (!firstChunkLogged) {
                        firstChunkLogged = true;
                        console.log(`[stream] first chunk ${fmtSize(r.value.byteLength)} in ${(performance.now()-t0).toFixed(0)}ms`);
                    }
                }
                console.log(`[stream] done ${fmtSize(totalBytes)} in ${(performance.now()-t0).toFixed(0)}ms`);
            }
        }
        if (myEpoch !== pbEpoch) return;

        // 走到这里说明 idx 路径未命中，缓存 / 流式 / fallback 都已经把整片字节塞进
        // SourceBuffer 了（前面任意一条出错会 return / break，不会走到这）。
        // 标记真理之源，让 tryAdvanceToNext 可以放心切下一片。
        pbAllAppended = true;

        // 关键：endOfStream 之后 MSE 才把所有 moof 的 sample 累加成 duration，
        // 进度条/拖动才能生效
        try { if (ms.readyState === 'open') ms.endOfStream(); } catch(e){}

        // 兜底 seek：极端情况下 buffered 在 append 中没命中（例如 ranges 不连续），
        //   下完之后再尝试一次。
        tryDoSeek();

        // 兜底 play（loadeddata 没触发 / 走 fallback arrayBuffer 路径时用得上）
        attemptPlaybackPlay();

        // 真实时长出来后用它修正 pbSegments[idx].dur（进度时间轴会更准）
        const fixDur = () => {
            if (myEpoch !== pbEpoch) return;
            if (isFinite(pbVideo.duration) && pbVideo.duration > 0) {
                seg.dur = Math.max(1, Math.round(pbVideo.duration));
                drawTimeline();
            }
        };
        pbVideo.addEventListener('durationchange', fixDur, { once:true });
        pbVideo.addEventListener('loadedmetadata', fixDur, { once:true });
    }
    segList.addEventListener('click', (ev) => {
        // 点击下载按钮：交由统一下载函数处理；录像走浏览器原生流式下载，
        // 避免完整 MP4 进入页面内存。
        const dl = ev.target.closest('.dl');
        if (dl) {
            ev.preventDefault();
            ev.stopPropagation();
            const href = dl.getAttribute('href') || '';
            const fname = dl.getAttribute('download') || '';
            downloadViaFetch(href, fname, (msg) => { pbInfo.textContent = msg; });
            return;
        }
        const it = ev.target.closest('.it');
        if (!it) return;
        playSeg(Number(it.getAttribute('data-idx')));
    });
    pbDate.addEventListener('change', () => {
        prefetchCache.clear();
        if (prefetchInflight) {
            try { prefetchInflight.ctrl.abort(); } catch(e){}
            prefetchInflight = null;
        }
        loadSegments(pbDate.value);
    });
    pbRefresh.addEventListener('click', loadDays);
    pbPrev.addEventListener('click', () => playSeg(pbCurIdx - 1));
    pbNext.addEventListener('click', () => playSeg(pbCurIdx + 1));
    // —— 播完一片自动切下一片 ——
    //
    // 之前几版反复修不好的根本原因：用 currentTime / buffered 这种"播放器外部"
    // 信号去推断"播完没"，但播放器分不清以下两种 stall：
    //   A) 播放头之后的 fragment 都已 append，currentTime 真到末尾  → 该切
    //   B) lookahead / seek refill 还没到，currentTime 暂时越过了 buffered  → 别切
    //
    // 真理之源策略：
    //   - "整片 append 完" pbAllAppended 太严（需要 backfill 中段所有空洞，
    //     用户拖来拖去后空洞很多，会被无限拖延）
    //   - 改用更精确的"末段已 append"信号：pbAppendedFrags 已包含最后一个
    //     fragment（N-1），并且 currentTime 接近最后一个 fragment 的末端
    //     这才是"该切"的真实条件，与中段是否有洞无关
    //
    // nearEnd 判定（OR 任一）：
    //   - pbVideo.ended（理想路径）
    //   - duration 有限且 currentTime >= duration - 0.4
    //   - currentTime >= buffered.end(last) - 0.4 持续 ≥ 600ms
    //     ★ 必须同时满足：要么 pbAllAppended=true（无 idx 路径走这条），
    //                    要么 idx 路径下 pbAppendedFrags 含 N-1 且 cur 已进入末段
    let advancing = false;
    let endStallSince = 0;       // currentTime 到达 buffered 末端的时刻（ms）
    let pbLastCur = -1;          // 上次观测的 currentTime，用于检测"是否还在前进"

    // 判断当前播放头是否处于"末段已就绪"状态：
    //   - 非 idx 路径（缓存 / 流式）：直接看 pbAllAppended（整片字节都进 sb 了）
    //   - idx 路径：最后一个 fragment 已 append，且播放头满足以下任一：
    //       a) cur >= 末 fragment 起点（正常播放：平滑推进自然越过）
    //       b) cur 已 stall 在 buffered 连续区末端附近（高倍速：大步跳进后停在
    //          buffered 尽头，位置可能恰好落在末 fragment 起点之前的浮点临界，
    //          cur 不再前进 → a) 永远差一点点判不过 → 永不切片。用 buffered 末端
    //          兜底：既然 N-1 在手、播放头又停在连续 buffered 的尽头，就是真播完）
    //     中段空洞不影响判定，因为播放头早已经过了它们
    function isPlayheadAtTrueEnd() {
        if (pbAllAppended) return true;
        if (!pbCurIdxJson || !pbAppendedFrags) return false;
        const N = pbCurIdxJson.frags.length;
        if (N <= 0) return false;
        if (!pbAppendedFrags.has(N - 1)) return false;
        const ts = pbCurIdxJson.ts || 90000;
        const lastStart = pbCurIdxJson.frags[N - 1][0] / ts;
        const cur = pbVideo.currentTime || 0;
        if (cur >= lastStart - 0.4) return true;            // a) 正常播放
        // b) 高倍速兜底：播放头已落在最后一段 buffered range 内（不论距末端多远）。
        //    高倍速 stall 位置距 bEnd 不固定，故用"在末段 range 内"判断而非固定距离。
        const b = pbVideo.buffered;
        if (b && b.length > 0) {
            const bEnd   = b.end(b.length - 1);
            const bStart = b.start(b.length - 1);
            if (bEnd > 0.5 && cur >= bStart - 0.1 && cur <= bEnd + 0.1) return true;
        }
        return false;
    }

    function tryAdvanceToNext(reason) {
        if (advancing) return;
        if (pbCurIdx < 0 || pbCurIdx >= pbSegments.length - 1) return;
        // ★ 核心守卫：播放头未到末段 → 任何末端 stall 都是续传未到，绝不切
        if (!isPlayheadAtTrueEnd()) { endStallSince = 0; return; }

        const dur = pbVideo.duration;
        const cur = pbVideo.currentTime || 0;

        let why = null;
        if (pbVideo.ended) {
            why = 'video.ended';
        } else if (isFinite(dur) && dur > 0 && cur >= dur - 0.4) {
            why = `near duration (${cur.toFixed(2)}/${dur.toFixed(2)})`;
        } else {
            const b = pbVideo.buffered;
            if (b && b.length > 0) {
                const bEnd   = b.end(b.length - 1);
                const bStart = b.start(b.length - 1);
                // 判定"停在最后一段 buffered 内且不再前进"，而非"距末端固定距离"。
                // 背景：高倍速越高，播放头 stall 的位置距 bEnd 越远（4x≈0.47s，
                // 16x≈0.88s…），固定容差总会被更高倍速突破（打地鼠）。
                // 稳健判据：播放头落在最后一段 buffered range 内（bStart..bEnd），
                // 且 currentTime 在 ≥600ms 内几乎没前进（stall）。配合
                // isPlayheadAtTrueEnd（N-1 已 append）双保险，就是真播完。
                const insideLastRange = (cur >= bStart - 0.1 && cur <= bEnd + 0.1);
                const now = performance.now();
                if (insideLastRange && bEnd > 0.5) {
                    const moved = Math.abs(cur - pbLastCur) > 0.05;
                    pbLastCur = cur;
                    if (moved) {
                        // 还在前进，不是 stall；但若已非常接近 bEnd 也允许起算
                        if (cur < bEnd - 0.8) { endStallSince = 0; }
                        else if (endStallSince === 0) endStallSince = now;
                    } else {
                        if (endStallSince === 0) endStallSince = now;
                    }
                    if (endStallSince !== 0 && now - endStallSince >= 600) {
                        why = `stall in last range (${cur.toFixed(2)} in ` +
                              `${bStart.toFixed(2)}..${bEnd.toFixed(2)}` +
                              `, ${(now - endStallSince).toFixed(0)}ms)`;
                    }
                } else {
                    endStallSince = 0;
                    pbLastCur = cur;
                }
            }
        }
        if (!why) return;

        advancing = true;
        endStallSince = 0;
        console.log(`[auto-next] advance via ${reason}: ${why}`);
        setTimeout(() => {
            advancing = false;
            if (pbCurIdx >= 0 && pbCurIdx < pbSegments.length - 1) {
                playSeg(pbCurIdx + 1);
            }
        }, 0);
    }
    pbVideo.addEventListener('ended',      () => tryAdvanceToNext('ended'));
    pbVideo.addEventListener('waiting',    () => {
        // 卡住时优先"继续补最后的 fragment"——若末段还没 append，光调 tryAdvanceToNext
        // 会被 isPlayheadAtTrueEnd 守卫挡掉，导致永远停在末尾。先补片，N-1 到位后再切。
        if (pbEnsureLookahead) { try { pbEnsureLookahead(); } catch(e){} }
        tryAdvanceToNext('waiting');
    });
    pbVideo.addEventListener('timeupdate', () => {
        drawTimeline();   // 更新红色播放游标
        maybeStartPrefetch();
        tryAdvanceToNext('timeupdate');
    });
    // 500ms 兜底轮询：即使所有 video 事件都不发，也能补片 + 切片。
    // 补片放在切片前：stall 在末尾时 timeupdate 停发，靠这个轮询继续把最后的
    // fragment 拉完，N-1 到位后 isPlayheadAtTrueEnd 转 true，下一拍即可切。
    setInterval(() => {
        if (curTab !== 'playback' || document.hidden) return;
        if (pbEnsureLookahead) { try { pbEnsureLookahead(); } catch(e){} }
        tryAdvanceToNext('poll');
    }, 500);

    // 自动切换诊断辅助：每 2s 在 pbAllAppended=false 时打印当前进度状态。
    // 用户报"无法自动切下一片"时直接看 console 这条 log 就能判断卡在哪：
    //   - allAppended=false + appended<N：lookahead 没追到末尾（看 nextFrag/未 append 列表）
    //   - allAppended=true 但不切：tryAdvanceToNext 守卫问题（看 dur/cur/bEnd）
    setInterval(() => {
        if (curTab !== 'playback' || document.hidden || pbCurIdx < 0) return;
        const dur = pbVideo.duration;
        const cur = pbVideo.currentTime || 0;
        const b = pbVideo.buffered;
        const bEnd = (b && b.length > 0) ? b.end(b.length - 1) : -1;
        let info = `[diag] seg#${pbCurIdx}/${pbSegments.length-1} ` +
                   `cur=${cur.toFixed(1)} bEnd=${bEnd.toFixed(1)} ` +
                   `dur=${isFinite(dur)?dur.toFixed(1):'∞'} ` +
                   `allApp=${pbAllAppended} adv=${advancing}`;
        if (pbCurIdxJson && pbAppendedFrags) {
            const N = pbCurIdxJson.frags.length;
            info += ` frags=${pbAppendedFrags.size}/${N}`;
            if (pbAppendedFrags.size < N) {
                let miss = [];
                for (let i = 0; i < N && miss.length < 5; i++) {
                    if (!pbAppendedFrags.has(i)) miss.push(i);
                }
                info += ` miss=[${miss.join(',')}${miss.length>=5?',…':''}]`;
            }
        }
        info += ` paused=${pbVideo.paused} ended=${pbVideo.ended} ` +
                `msState=${pbMs?pbMs.readyState:'-'}`;
        console.log(info);
    }, 2000);

    // 当前片快结束时（dur - 5s）仅对不超过 8 MiB 的小片后台预拉。
    //
    // 触发条件全部满足时启动：
    //   1) 当前片有 idx 且非最后一片
    //   2) 当前片本身已被加载过（duration 已知，否则 currentTime 不可信）
    //   3) currentTime > duration - 5s
    //   4) 没正在拉别的预拉、且目标 url 不在缓存里
    //
    // 失败 / 中止都是静默的：未拉到只是切片时退化成正常下载，不影响功能。
    function maybeStartPrefetch() {
        if (prefetchTriggered) return;
        if (pbCurIdx < 0 || pbCurIdx >= pbSegments.length - 1) return;
        const dur = pbVideo.duration;
        if (!isFinite(dur) || dur <= 0) return;
        if (pbVideo.currentTime < Math.max(0, dur - 5)) return;

        const date = pbDate.value;
        const next = pbSegments[pbCurIdx + 1];
        const url  = '/record/' + encodeURIComponent(date) + '/' + encodeURIComponent(next.name);
        if (!next.size || next.size > PREFETCH_MAX_BYTES) {
            prefetchTriggered = true;
            return;
        }
        if (prefetchCache.has(url)) { prefetchTriggered = true; return; }
        if (prefetchInflight && prefetchInflight.url === url) return;

        prefetchTriggered = true;
        const ctrl = new AbortController();
        prefetchInflight = { url, ctrl };
        const t0 = performance.now();
        fetch(url, { signal: ctrl.signal, cache: 'no-store' })
            .then(r => r.ok ? r.arrayBuffer() : Promise.reject('http ' + r.status))
            .then(buf => {
                if (buf.byteLength > PREFETCH_MAX_BYTES) return;
                // 单条 LRU：超出容量删最早的
                while (prefetchCache.size >= PREFETCH_LRU_SIZE) {
                    const firstKey = prefetchCache.keys().next().value;
                    prefetchCache.delete(firstKey);
                }
                prefetchCache.set(url, { buf, size: buf.byteLength });
                const ms = (performance.now() - t0).toFixed(0);
                console.log(`[prefetch] ok ${fmtSize(buf.byteLength)} in ${ms}ms ${next.name}`);
            })
            .catch(err => {
                if (err && err.name === 'AbortError') return;
                console.warn('[prefetch] fail', next.name, err);
            })
            .finally(() => {
                if (prefetchInflight && prefetchInflight.url === url) {
                    prefetchInflight = null;
                }
            });
    }

    // ---------- 24h 时间轴 ----------
    function resizeTimeline() {
        // 让 canvas 像素与显示宽度对齐，避免高 DPI 模糊
        const dpr = window.devicePixelRatio || 1;
        const cssW = tlCanvas.clientWidth || tlCanvas.parentElement.clientWidth || 800;
        const cssH = window.innerWidth <= 640 ? 58 : 64;
        tlCanvas.width  = Math.max(200, Math.floor(cssW * dpr));
        tlCanvas.height = Math.floor(cssH * dpr);
        tlCanvas.style.height = cssH + 'px';
        drawTimeline();
    }
    function drawTimeline() {
        const ctx = tlCanvas.getContext('2d');
        const W = tlCanvas.width, H = tlCanvas.height;
        const DAY = 86400;
        ctx.clearRect(0, 0, W, H);
        // 背景刻度
        ctx.fillStyle = '#07141f'; ctx.fillRect(0, 0, W, H);
        ctx.strokeStyle = 'rgba(160,190,210,.18)'; ctx.fillStyle = '#8fa4b1';
        ctx.font = (12 * (window.devicePixelRatio||1)) + 'px monospace';
        ctx.textBaseline = 'top';
        for (let h = 0; h <= 24; h++) {
            const x = Math.round(W * (h / 24));
            ctx.beginPath();
            ctx.moveTo(x, H - (h % 6 === 0 ? 18 : 10));
            ctx.lineTo(x, H);
            ctx.stroke();
            if (h % 3 === 0 && h < 24) {
                ctx.fillText((h<10?'0':'') + h + ':00', x + 4, 4);
            }
        }
        // 录像色块
        ctx.fillStyle = '#36c7b4';
        for (const s of pbSegments) {
            const x1 = (s.sec / DAY) * W;
            const x2 = ((s.sec + (s.dur||600)) / DAY) * W;
            ctx.fillRect(x1, 18, Math.max(1, x2 - x1), H - 28);
        }
        // 当前片高亮 + 播放游标
        if (pbCurIdx >= 0 && pbCurIdx < pbSegments.length) {
            const s = pbSegments[pbCurIdx];
            ctx.strokeStyle = '#dffcf7'; ctx.lineWidth = 2;
            const x1 = (s.sec / DAY) * W;
            const x2 = ((s.sec + (s.dur||600)) / DAY) * W;
            ctx.strokeRect(x1, 18, Math.max(1, x2 - x1), H - 28);
            // 播放游标
            const t = pbVideo.currentTime || 0;
            const cursorSec = s.sec + Math.min(t, s.dur || 600);
            const cx = (cursorSec / DAY) * W;
            ctx.strokeStyle = '#f07178'; ctx.lineWidth = 2;
            ctx.beginPath(); ctx.moveTo(cx, 0); ctx.lineTo(cx, H); ctx.stroke();
        }
    }
    function tlSecAt(clientX) {
        const rect = tlCanvas.getBoundingClientRect();
        const ratio = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
        return Math.floor(ratio * 86400);
    }
    function findSegAtSec(sec) {
        for (let i = 0; i < pbSegments.length; i++) {
            const s = pbSegments[i];
            if (sec >= s.sec && sec < s.sec + (s.dur || 600)) return i;
        }
        return -1;
    }
    tlCanvas.addEventListener('click', (ev) => {
        const sec = tlSecAt(ev.clientX);
        const idx = findSegAtSec(sec);
        if (idx >= 0) {
            const off = sec - pbSegments[idx].sec;
            playSeg(idx, off);
        } else {
            // 点空白：跳到时间点之后的第一段（找第一个 sec >= clickSec）
            const next = pbSegments.findIndex(s => s.sec >= sec);
            if (next >= 0) playSeg(next, 0);
        }
    });
    tlCanvas.addEventListener('mousemove', (ev) => {
        const sec = tlSecAt(ev.clientX);
        const hh = Math.floor(sec/3600), mm = Math.floor((sec/60)%60), ss = sec%60;
        const idx = findSegAtSec(sec);
        const txt = `${(hh<10?'0':'')+hh}:${(mm<10?'0':'')+mm}:${(ss<10?'0':'')+ss}` +
                    (idx >= 0 ? ` · 第 ${idx+1}/${pbSegments.length} 段` : ' · 无录像');
        tlTip.textContent = txt;
        tlTip.hidden = false;
        const rect = tlCanvas.getBoundingClientRect();
        tlTip.style.left = (ev.clientX - rect.left) + 'px';
        tlTip.style.top  = '0px';
    });
    tlCanvas.addEventListener('mouseleave', () => { tlTip.hidden = true; });

    // ============================================================
    // 相册 Tab：网格 + 多选 + 删除 + 大图查看
    // ============================================================
    let albumInited = false;
    let albumDirty  = false;   // 拍完照后标 true，下次进入相册自动刷新
    let albumItems  = [];      // [{name,size,mtime}]
    const selected  = new Set();

    const abRefresh  = document.getElementById('abRefresh');
    const abSelAll   = document.getElementById('abSelAll');
    const abSelNone  = document.getElementById('abSelNone');
    const abDownload = document.getElementById('abDownload');
    const abDelete   = document.getElementById('abDelete');
    const abStat     = document.getElementById('abStat');
    const albumGrid  = document.getElementById('albumGrid');
    const albumEmpty = document.getElementById('albumEmpty');
    const lightbox   = document.getElementById('lightbox');
    const lbImg      = document.getElementById('lbImg');
    const lbDownload = document.getElementById('lbDownload');
    const lbClose    = document.getElementById('lbClose');

    function setAlbumStatus(text, state) {
        setChip(abStat, text, state || 'idle');
    }

    function closeLightbox() {
        lightbox.classList.remove('open');
        lightbox.setAttribute('aria-hidden', 'true');
        lbImg.src = '';
        lbDownload.removeAttribute('href');
    }

    function initAlbum() {
        albumInited = true;
        abRefresh.onclick  = () => loadAlbum();
        abSelAll.onclick   = () => { for (const it of albumItems) selected.add(it.name); renderAlbum(); };
        abSelNone.onclick  = () => { selected.clear(); renderAlbum(); };
        abDownload.onclick = onDownloadSelected;
        abDelete.onclick   = onDeleteSelected;
        // lightbox 背景点击关闭；但下载按钮上的点击要 stopPropagation，
        // 否则点"下载"会先触发 a[download]、再冒泡到 lightbox 关闭，体验不连贯
        lbDownload.addEventListener('click', (ev) => {
            ev.stopPropagation();
            ev.preventDefault();
            const href  = lbDownload.getAttribute('href') || '';
            const fname = lbDownload.getAttribute('download') || '';
            if (!href || href === '#') return;
            // 大图下载：走 fetch+Blob 而不是 a[download] 原生跳转，
            // 否则 https 自签名时 Chrome 会以"网络错误"失败
            downloadViaFetch(href, fname, (msg) => { setAlbumStatus(msg, 'busy'); });
        });
        lightbox.addEventListener('click', (ev) => {
            if (ev.target === lightbox || ev.target === lbImg) closeLightbox();
        });
        lbClose.addEventListener('click', closeLightbox);
    }

    async function refreshAlbum() {
        if (albumDirty || albumItems.length === 0) {
            albumDirty = false;
            await loadAlbum();
        }
    }

    async function loadAlbum() {
        setAlbumStatus('加载中', 'busy');
        const r = await api('GET', '/api/photo/list?ts=' + Date.now());
        if (!r.ok) { setAlbumStatus(r.error || '加载失败', 'error'); return; }
        albumItems = Array.isArray(r.data) ? r.data : [];
        // 清理已被删掉的选中项
        for (const n of [...selected]) if (!albumItems.find(it => it.name === n)) selected.delete(n);
        renderAlbum();
    }

    function fmtTime(yyyymmdd_hhmmss_NNN) {
        // 文件名形如 20260523_141502_001.jpg
        const m = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})_(\d{3})\.jpg$/.exec(yyyymmdd_hhmmss_NNN || '');
        if (!m) return yyyymmdd_hhmmss_NNN || '';
        return `${m[1]}-${m[2]}-${m[3]} ${m[4]}:${m[5]}:${m[6]}`;
    }

    function renderAlbum() {
        if (albumItems.length === 0) {
            albumGrid.innerHTML = '';
            albumEmpty.hidden = false;
            setAlbumStatus('共 0 张', 'idle');
            updateDelBtn();
            return;
        }
        albumEmpty.hidden = true;
        const html = albumItems.map(it => {
            const sel = selected.has(it.name) ? 'sel' : '';
            const url = '/photo/' + encodeURIComponent(it.name);
            return `<div class="ph ${sel}" data-name="${escapeHtml(it.name)}">
                <img loading="lazy" src="${escapeHtml(url)}" alt="${escapeHtml(it.name)}">
                <div class="chk"></div>
                <div class="meta">${escapeHtml(fmtTime(it.name))} · ${fmtSize(it.size)}</div>
            </div>`;
        }).join('');
        albumGrid.innerHTML = html;
        setAlbumStatus(`共 ${albumItems.length} 张 · 已选 ${selected.size}`,
                       selected.size ? 'playing' : 'idle');
        updateDelBtn();
    }
    function updateDelBtn() {
        const empty = selected.size === 0;
        abDelete.disabled   = empty;
        abDownload.disabled = empty;
    }

    albumGrid && (albumGrid.onclick = (ev) => {
        const ph = ev.target.closest('.ph');
        if (!ph) return;
        const name = ph.getAttribute('data-name');
        // 点击选择框 → 切换选中；点击图片其他部分 → 大图查看
        if (ev.target.closest('.chk')) {
            if (selected.has(name)) selected.delete(name);
            else                    selected.add(name);
            ph.classList.toggle('sel');
            setAlbumStatus(`共 ${albumItems.length} 张 · 已选 ${selected.size}`,
                           selected.size ? 'playing' : 'idle');
            updateDelBtn();
            return;
        }
        // 否则打开 lightbox
        const url = '/photo/' + encodeURIComponent(name);
        lbImg.src = url;
        // 同步下载按钮指向当前查看的图片，浏览器看到 a[download] 会触发"另存为"
        lbDownload.href = url;
        lbDownload.setAttribute('download', name);
        lightbox.classList.add('open');
        lightbox.setAttribute('aria-hidden', 'false');
    });

    async function onDeleteSelected() {
        if (selected.size === 0) return;
        const names = [...selected];
        if (!await askConfirm(`确定删除 ${names.length} 张照片吗？此操作不可撤销。`)) return;
        abDelete.disabled = true;
        setAlbumStatus('删除中', 'busy');
        try {
            const r = await api('POST', '/api/photo/delete', { names });
            if (!r.ok || !r.data || !r.data.ok) {
                const message = '删除失败：' + (r.error || ('HTTP ' + r.status));
                setAlbumStatus(message, 'error');
                showToast(message, 'error');
                return;
            }
            const d = r.data;
            // 极端兜底：服务端返回 ok 但什么都没删（旧版 httpServer 在 body 跨 TCP 段时会
            // 漏读，导致 names 解析为空）；提示用户而不是装作成功
            if (d.deleted === 0 && (d.missing || 0) === 0 &&
                (!d.errors || d.errors.length === 0) && names.length > 0) {
                const message = '删除未生效：服务端未识别到文件名，请检查后端版本';
                setAlbumStatus(message, 'error');
                showToast(message, 'error');
                return;
            }
            selected.clear();
            await loadAlbum();
            const message = `已删除 ${d.deleted} 张` +
                (d.missing ? `（${d.missing} 张已不存在）` : '') +
                (d.errors && d.errors.length ? `，错误 ${d.errors.length}` : '');
            setAlbumStatus(message, d.errors && d.errors.length ? 'error' : 'playing');
            showToast(message, d.errors && d.errors.length ? 'error' : '');
        } catch (e) {
            console.error('delete photos fail:', e);
            const message = '删除失败：' + (e && e.message ? e.message : e);
            setAlbumStatus(message, 'error');
            showToast(message, 'error');
        } finally {
            updateDelBtn();
        }
    }

    // 多选批量下载：串行 fetch+Blob 触发每张图保存。
    //
    // 走 fetch 而不是 a[download] 的原因：
    //   https + 自签名证书时，<a download> 触发的下载会被 Chrome 视为新的安全场景
    //   重新校验证书，进而以 net::ERR_CERT_AUTHORITY_INVALID 失败（页面里看到
    //   "失败 - 网络错误"）；而 fetch 复用页面 Tab 已忽略警告的 TLS 通道，能稳定下载。
    //
    // 浏览器侧的限制：
    //   1) Chrome/Edge 默认会拦截"短时间内多次自动下载"，第一次会弹一个"该网站尝试下载多个文件"
    //      的横幅，用户点"允许"后续才能放行；这是浏览器主动行为，前端无法绕过；
    //   2) 我们用 await 串行 + setTimeout 间隔 ~150ms，降低被聚合拦截的概率；
    //   3) 文件较大时浏览器会把整张图先放进内存再 saveAs，对单张图（< 数 MB）影响不大。
    async function onDownloadSelected() {
        if (selected.size === 0) return;
        const names = [...selected];
        abDownload.disabled = true;
        let okCnt = 0, failCnt = 0;
        for (let i = 0; i < names.length; i++) {
            const name = names[i];
            setAlbumStatus(`下载中 ${i + 1}/${names.length}`, 'busy');
            const ok = await downloadViaFetch('/photo/' + encodeURIComponent(name), name, null);
            if (ok) okCnt++; else failCnt++;
            // 间隔一下，规避浏览器"多文件下载"聚合拦截
            if (i < names.length - 1) {
                await new Promise(r => setTimeout(r, 150));
            }
        }
        const message =
            `下载完成 · 成功 ${okCnt}` +
            (failCnt ? ` · 失败 ${failCnt}` : '') +
            (names.length > 1 ? '（首次浏览器可能弹"允许多个文件下载"提示）' : '');
        setAlbumStatus(message, failCnt ? 'error' : 'playing');
        showToast(`下载完成：成功 ${okCnt}` + (failCnt ? `，失败 ${failCnt}` : ''),
                  failCnt ? 'error' : '');
        updateDelBtn();
    }

    // ============================================================
    // 设置 Tab：GET /api/config 加载，POST /api/config 保存
    //
    // - 字段命名与后端 C_AppConfig::Snapshot 完全一致；用 id="cfg_<key>" 约定批量绑定
    // - record_max_bytes 在 UI 上以 GB 展示；保存时换算回 byte
    // - 保存只发"实际改过"的字段（patch 语义），但实现上简化为全量提交，
    //   后端 ApplyPatch 容忍未变化字段（写盘开销可忽略）
    // ============================================================
    let cfgInited = false;
    let cfgDirty = false;
    const cfgEl = {};
    const cfgIds = [
        'cfg_ai_enabled', 'cfg_ai_threshold',
        'cfg_ai_min_interval_s', 'cfg_ai_infer_fps',
        'cfg_vmd_enabled', 'cfg_vmd_pixel_thresh', 'cfg_vmd_area_ratio',
        'cfg_vmd_min_interval_s', 'cfg_vmd_check_fps',
        'cfg_record_segment_s', 'cfg_record_retain_days', 'cfg_record_max_gb',
        'cfg_album_max_photos', 'cfg_photo_jpeg_qual', 'cfg_mic_filter_mode',
        'cfg_osd_show_ip', 'cfg_osd_show_time', 'cfg_osd_show_ai_box',
        'cfg_light_enabled', 'cfg_light_mode', 'cfg_light_start_hour',
        'cfg_light_end_hour', 'cfg_light_sound_thresh', 'cfg_light_hold_s',
        'cfg_light_gpio', 'cfg_light_active_low',
        'cfg_mqtt_enabled', 'cfg_mqtt_broker_host', 'cfg_mqtt_broker_port',
        'cfg_mqtt_topic', 'cfg_mqtt_client_id', 'cfg_mqtt_poll_sec', 'cfg_mqtt_iface',
        'cfg_mqtt_report_interval_s', 'cfg_mqtt_retain'
    ];
    const cfgStat = () => document.getElementById('cfgStat');
    const cfgSave = () => document.getElementById('cfgSave');

    function setConfigStatus(text, state) {
        setChip(cfgStat(), text, state || 'idle');
    }

    function initSettings() {
        cfgInited = true;
        for (const id of cfgIds) cfgEl[id] = document.getElementById(id);
        document.getElementById('cfgReload').addEventListener('click', async () => {
            if (cfgDirty && !await askConfirm('重新加载会丢弃尚未保存的设置，是否继续？')) {
                return;
            }
            loadConfig();
        });
        cfgSave().addEventListener('click', saveConfig);
        panes.settings.addEventListener('input', () => {
            if (cfgDirty) return;
            cfgDirty = true;
            setConfigStatus('有未保存更改', 'busy');
        });
        loadConfig();
    }
    window.addEventListener('beforeunload', ev => {
        if (!cfgDirty) return;
        ev.preventDefault();
        ev.returnValue = '';
    });

    function fillForm(c) {
        cfgEl.cfg_ai_enabled.checked         = !!c.ai_enabled;
        cfgEl.cfg_ai_threshold.value         = c.ai_threshold;
        cfgEl.cfg_ai_min_interval_s.value    = c.ai_min_interval_s;
        cfgEl.cfg_ai_infer_fps.value         = c.ai_infer_fps;
        cfgEl.cfg_vmd_enabled.checked        = !!c.vmd_enabled;
        cfgEl.cfg_vmd_pixel_thresh.value     = (c.vmd_pixel_thresh   !== undefined) ? c.vmd_pixel_thresh   : 25;
        cfgEl.cfg_vmd_area_ratio.value       = (c.vmd_area_ratio     !== undefined) ? c.vmd_area_ratio     : 0.02;
        cfgEl.cfg_vmd_min_interval_s.value   = (c.vmd_min_interval_s !== undefined) ? c.vmd_min_interval_s : 2;
        cfgEl.cfg_vmd_check_fps.value        = (c.vmd_check_fps      !== undefined) ? c.vmd_check_fps      : 5;
        cfgEl.cfg_record_segment_s.value     = c.record_segment_s;
        cfgEl.cfg_record_retain_days.value   = c.record_retain_days;
        // 后端是 byte，UI 展示 GB（向上取整到整 GB；不足 1GB 显 1）
        const gb = Math.max(1, Math.round(Number(c.record_max_bytes) / (1024*1024*1024)));
        cfgEl.cfg_record_max_gb.value        = gb;
        cfgEl.cfg_album_max_photos.value     = c.album_max_photos;
        cfgEl.cfg_photo_jpeg_qual.value      = c.photo_jpeg_qual;
        cfgEl.cfg_mic_filter_mode.value      = (c.mic_filter_mode !== undefined) ? c.mic_filter_mode : 0;
        cfgEl.cfg_osd_show_ip.checked        = !!c.osd_show_ip;
        cfgEl.cfg_osd_show_time.checked      = !!c.osd_show_time;
        cfgEl.cfg_osd_show_ai_box.checked    = (c.osd_show_ai_box !== undefined) ? !!c.osd_show_ai_box : true;
        cfgEl.cfg_light_enabled.checked      = !!c.light_enabled;
        cfgEl.cfg_light_mode.value           = (c.light_mode         !== undefined) ? c.light_mode         : 0;
        cfgEl.cfg_light_start_hour.value     = (c.light_start_hour   !== undefined) ? c.light_start_hour   : 18;
        cfgEl.cfg_light_end_hour.value       = (c.light_end_hour     !== undefined) ? c.light_end_hour     : 6;
        cfgEl.cfg_light_sound_thresh.value   = (c.light_sound_thresh !== undefined) ? c.light_sound_thresh : 35;
        cfgEl.cfg_light_hold_s.value         = (c.light_hold_s       !== undefined) ? c.light_hold_s       : 30;
        cfgEl.cfg_light_gpio.value           = (c.light_gpio         !== undefined) ? c.light_gpio         : 237;
        cfgEl.cfg_light_active_low.checked   = (c.light_active_low   !== undefined) ? !!c.light_active_low : false;
        cfgEl.cfg_mqtt_enabled.checked       = !!c.mqtt_enabled;
        cfgEl.cfg_mqtt_broker_host.value     = (c.mqtt_broker_host !== undefined) ? c.mqtt_broker_host : 'broker.emqx.io';
        cfgEl.cfg_mqtt_broker_port.value     = (c.mqtt_broker_port !== undefined) ? c.mqtt_broker_port : 1883;
        cfgEl.cfg_mqtt_topic.value           = (c.mqtt_topic       !== undefined) ? c.mqtt_topic       : 'cam/ipv6';
        cfgEl.cfg_mqtt_client_id.value       = (c.mqtt_client_id   !== undefined) ? c.mqtt_client_id   : 'v831cam';
        cfgEl.cfg_mqtt_poll_sec.value        = (c.mqtt_poll_sec    !== undefined) ? c.mqtt_poll_sec    : 10;
        cfgEl.cfg_mqtt_iface.value           = (c.mqtt_iface       !== undefined) ? c.mqtt_iface       : 'wlan0';
        cfgEl.cfg_mqtt_report_interval_s.value = (c.mqtt_report_interval_s !== undefined) ? c.mqtt_report_interval_s : 3600;
        cfgEl.cfg_mqtt_retain.checked        = (c.mqtt_retain !== undefined) ? !!c.mqtt_retain : true;
        cfgDirty = false;
    }

    async function loadConfig() {
        setConfigStatus('加载中', 'busy');
        cfgSave().disabled = true;
        const r = await api('GET', '/api/config');
        cfgSave().disabled = false;
        if (!r.ok) {
            setConfigStatus(r.error || ('加载失败 ' + r.status), 'error');
            return;
        }
        try {
            fillForm(r.data);
            setConfigStatus('设置已同步', 'playing');
        } catch (e) {
            setConfigStatus('解析失败：' + e.message, 'error');
        }
    }

    async function saveConfig() {
        const invalid = cfgIds.map(id => cfgEl[id]).find(el => el && !el.checkValidity());
        if (invalid) {
            invalid.reportValidity();
            invalid.focus();
            return;
        }
        const gb = parseInt(cfgEl.cfg_record_max_gb.value, 10) || 1;
        const payload = {
            ai_enabled:         cfgEl.cfg_ai_enabled.checked,
            ai_threshold:       parseFloat(cfgEl.cfg_ai_threshold.value),
            ai_min_interval_s:  parseInt(cfgEl.cfg_ai_min_interval_s.value, 10),
            ai_infer_fps:       parseInt(cfgEl.cfg_ai_infer_fps.value, 10),
            vmd_enabled:        cfgEl.cfg_vmd_enabled.checked,
            vmd_pixel_thresh:   parseInt(cfgEl.cfg_vmd_pixel_thresh.value, 10),
            vmd_area_ratio:     parseFloat(cfgEl.cfg_vmd_area_ratio.value),
            vmd_min_interval_s: parseInt(cfgEl.cfg_vmd_min_interval_s.value, 10),
            vmd_check_fps:      parseInt(cfgEl.cfg_vmd_check_fps.value, 10),
            record_segment_s:   parseInt(cfgEl.cfg_record_segment_s.value, 10),
            record_retain_days: parseInt(cfgEl.cfg_record_retain_days.value, 10),
            record_max_bytes:   gb * 1024 * 1024 * 1024,
            album_max_photos:   parseInt(cfgEl.cfg_album_max_photos.value, 10),
            photo_jpeg_qual:    parseInt(cfgEl.cfg_photo_jpeg_qual.value, 10),
            mic_filter_mode:    parseInt(cfgEl.cfg_mic_filter_mode.value, 10),
            osd_show_ip:        cfgEl.cfg_osd_show_ip.checked,
            osd_show_time:      cfgEl.cfg_osd_show_time.checked,
            osd_show_ai_box:    cfgEl.cfg_osd_show_ai_box.checked,
            light_enabled:      cfgEl.cfg_light_enabled.checked,
            light_mode:         parseInt(cfgEl.cfg_light_mode.value, 10),
            light_start_hour:   parseInt(cfgEl.cfg_light_start_hour.value, 10),
            light_end_hour:     parseInt(cfgEl.cfg_light_end_hour.value, 10),
            light_sound_thresh: parseInt(cfgEl.cfg_light_sound_thresh.value, 10),
            light_hold_s:       parseInt(cfgEl.cfg_light_hold_s.value, 10),
            light_gpio:         parseInt(cfgEl.cfg_light_gpio.value, 10),
            light_active_low:   cfgEl.cfg_light_active_low.checked,
            mqtt_enabled:       cfgEl.cfg_mqtt_enabled.checked,
            mqtt_broker_host:   cfgEl.cfg_mqtt_broker_host.value.trim(),
            mqtt_broker_port:   parseInt(cfgEl.cfg_mqtt_broker_port.value, 10),
            mqtt_topic:         cfgEl.cfg_mqtt_topic.value.trim(),
            mqtt_client_id:     cfgEl.cfg_mqtt_client_id.value.trim(),
            mqtt_poll_sec:      parseInt(cfgEl.cfg_mqtt_poll_sec.value, 10),
            mqtt_iface:         cfgEl.cfg_mqtt_iface.value.trim(),
            mqtt_report_interval_s: parseInt(cfgEl.cfg_mqtt_report_interval_s.value, 10),
            mqtt_retain:        cfgEl.cfg_mqtt_retain.checked,
        };
        if (Object.keys(payload).some(k =>
            typeof payload[k] === 'number' && !Number.isFinite(payload[k]))) {
            setConfigStatus('存在未填写或无效的数值', 'error');
            showToast('请检查设置中的数值字段', 'error');
            return;
        }
        setConfigStatus('保存中', 'busy');
        cfgSave().disabled = true;
        const r = await api('POST', '/api/config', payload);
        cfgSave().disabled = false;
        if (!r.ok) {
            setConfigStatus(r.error || ('保存失败 ' + r.status), 'error');
            showToast('设置保存失败', 'error');
            return;
        }
        if (r.data && r.data.config) {
            fillForm(r.data.config);   // 后端 clamp 后再回填
            setConfigStatus('已保存并校正到合法范围', 'playing');
        } else {
            cfgDirty = false;
            setConfigStatus('设置已保存', 'playing');
        }
        showToast('设置已保存');
    }

})();
