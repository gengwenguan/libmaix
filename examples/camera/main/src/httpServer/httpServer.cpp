/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  httpServer.cpp
  *Author:    gengwenguan
  *Date:      2024-12-01
  *Description:  HTTP server implementation (serving web_player.html)
**********************************************************************************/
#include "httpServer.h"
#include "logAdapt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <chrono>

// Embedded web_player.html content
static const char WEB_PLAYER_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"    <title>Camera WebSocket Player v3</title>\n"
"    <style>\n"
"        * {\n"
"            margin: 0;\n"
"            padding: 0;\n"
"            box-sizing: border-box;\n"
"        }\n"
"\n"
"        body {\n"
"            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
"            background: linear-gradient(135deg, #1e3c72 0%, #2a5298 100%);\n"
"            min-height: 100vh;\n"
"            display: flex;\n"
"            flex-direction: column;\n"
"            align-items: center;\n"
"            padding: 20px;\n"
"        }\n"
"\n"
"        h1 {\n"
"            color: white;\n"
"            margin-bottom: 20px;\n"
"            text-shadow: 2px 2px 4px rgba(0,0,0,0.3);\n"
"        }\n"
"\n"
"        .container {\n"
"            background: rgba(255, 255, 255, 0.1);\n"
"            backdrop-filter: blur(10px);\n"
"            border-radius: 20px;\n"
"            padding: 30px;\n"
"            box-shadow: 0 8px 32px rgba(0,0,0,0.3);\n"
"            max-width: 900px;\n"
"            width: 100%;\n"
"        }\n"
"\n"
"        .video-container {\n"
"            position: relative;\n"
"            width: 100%;\n"
"            max-width: 640px;\n"
"            margin: 0 auto 20px;\n"
"            background: #000;\n"
"            border-radius: 10px;\n"
"            overflow: hidden;\n"
"        }\n"
"\n"
"        #videoCanvas {\n"
"            width: 100%;\n"
"            height: auto;\n"
"            display: block;\n"
"            background-color: #000;\n"
"            border: 2px solid #0ff;\n"
"        }\n"
"\n"
"        .controls {\n"
"            display: flex;\n"
"            gap: 10px;\n"
"            justify-content: center;\n"
"            flex-wrap: wrap;\n"
"            margin-bottom: 20px;\n"
"        }\n"
"\n"
"        button {\n"
"            padding: 12px 24px;\n"
"            border: none;\n"
"            border-radius: 25px;\n"
"            cursor: pointer;\n"
"            font-size: 14px;\n"
"            font-weight: 600;\n"
"            transition: all 0.3s ease;\n"
"            text-transform: uppercase;\n"
"            letter-spacing: 0.5px;\n"
"        }\n"
"\n"
"        .btn-primary {\n"
"            background: linear-gradient(45deg, #00c6ff, #0072ff);\n"
"            color: white;\n"
"        }\n"
"\n"
"        .btn-primary:hover {\n"
"            transform: translateY(-2px);\n"
"            box-shadow: 0 5px 15px rgba(0,114,255,0.4);\n"
"        }\n"
"\n"
"        .btn-danger {\n"
"            background: linear-gradient(45deg, #ff416c, #ff4b2b);\n"
"            color: white;\n"
"        }\n"
"\n"
"        .btn-danger:hover {\n"
"            transform: translateY(-2px);\n"
"            box-shadow: 0 5px 15px rgba(255,65,108,0.4);\n"
"        }\n"
"\n"
"        .btn-secondary {\n"
"            background: rgba(255,255,255,0.2);\n"
"            color: white;\n"
"            border: 1px solid rgba(255,255,255,0.3);\n"
"        }\n"
"\n"
"        .btn-secondary:hover {\n"
"            background: rgba(255,255,255,0.3);\n"
"        }\n"
"\n"
"        .status {\n"
"            text-align: center;\n"
"            color: white;\n"
"            margin-bottom: 15px;\n"
"            padding: 10px;\n"
"            background: rgba(0,0,0,0.2);\n"
"            border-radius: 10px;\n"
"        }\n"
"\n"
"        .status.connected {\n"
"            background: rgba(0,255,0,0.2);\n"
"        }\n"
"\n"
"        .status.disconnected {\n"
"            background: rgba(255,0,0,0.2);\n"
"        }\n"
"\n"
"        .info-panel {\n"
"            background: rgba(0,0,0,0.2);\n"
"            border-radius: 10px;\n"
"            padding: 15px;\n"
"            color: white;\n"
"            font-family: 'Courier New', monospace;\n"
"            font-size: 12px;\n"
"            max-height: 200px;\n"
"            overflow-y: auto;\n"
"        }\n"
"\n"
"        .info-panel h3 {\n"
"            margin-bottom: 10px;\n"
"            color: #00c6ff;\n"
"        }\n"
"\n"
"        .log-entry {\n"
"            margin: 5px 0;\n"
"            padding: 5px;\n"
"            background: rgba(255,255,255,0.05);\n"
"            border-radius: 5px;\n"
"        }\n"
"\n"
"        .server-config {\n"
"            margin-bottom: 20px;\n"
"            text-align: center;\n"
"        }\n"
"\n"
"        .server-config input {\n"
"            padding: 10px 15px;\n"
"            border: none;\n"
"            border-radius: 25px;\n"
"            background: rgba(255,255,255,0.2);\n"
"            color: white;\n"
"            width: 250px;\n"
"            margin-right: 10px;\n"
"        }\n"
"\n"
"        .server-config input::placeholder {\n"
"            color: rgba(255,255,255,0.6);\n"
"        }\n"
"\n"
"        .stats {\n"
"            display: grid;\n"
"            grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));\n"
"            gap: 15px;\n"
"            margin-bottom: 20px;\n"
"        }\n"
"\n"
"        .stat-card {\n"
"            background: rgba(255,255,255,0.1);\n"
"            padding: 15px;\n"
"            border-radius: 10px;\n"
"            text-align: center;\n"
"            color: white;\n"
"        }\n"
"\n"
"        .stat-card h4 {\n"
"            font-size: 12px;\n"
"            opacity: 0.8;\n"
"            margin-bottom: 5px;\n"
"        }\n"
"\n"
"        .stat-card .value {\n"
"            font-size: 24px;\n"
"            font-weight: bold;\n"
"            color: #00c6ff;\n"
"        }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <h1>Camera WebSocket Player v3</h1>\n"
"\n"
"    <div class=\"container\">\n"
"        <div class=\"server-config\">\n"
"            <input type=\"text\" id=\"serverIp\" placeholder=\"Enter device IP address\" value=\"192.168.1.11\">\n"
"        </div>\n"
"\n"
"        <div class=\"video-container\">\n"
"            <canvas id=\"videoCanvas\" width=\"640\" height=\"480\"></canvas>\n"
"        </div>\n"
"\n"
"        <div class=\"status disconnected\" id=\"status\">\n"
"            Not connected - Click \"Start Live\" button\n"
"        </div>\n"
"\n"
"        <div class=\"stats\">\n"
"            <div class=\"stat-card\">\n"
"                <h4>Video FPS</h4>\n"
"                <div class=\"value\" id=\"fps\">0</div>\n"
"            </div>\n"
"            <div class=\"stat-card\">\n"
"                <h4>Audio FPS</h4>\n"
"                <div class=\"value\" id=\"audioFps\">0</div>\n"
"            </div>\n"
"            <div class=\"stat-card\">\n"
"                <h4>Bitrate</h4>\n"
"                <div class=\"value\" id=\"bitrate\">0</div>\n"
"            </div>\n"
"            <div class=\"stat-card\">\n"
"                <h4>Status</h4>\n"
"                <div class=\"value\" id=\"connectionStatus\">Offline</div>\n"
"            </div>\n"
"        </div>\n"
"\n"
"        <div class=\"controls\">\n"
"            <button class=\"btn-primary\" onclick=\"startLive()\">Start Live</button>\n"
"            <button class=\"btn-danger\" onclick=\"stopLive()\">Stop Live</button>\n"
"            <button class=\"btn-secondary\" onclick=\"startPlayback()\">Start Playback</button>\n"
"            <button class=\"btn-secondary\" onclick=\"stopPlayback()\">Stop Playback</button>\n"
"        </div>\n"
"\n"
"        <div class=\"controls\">\n"
"            <button class=\"btn-secondary\" onclick=\"sendCommand(101)\">Rewind</button>\n"
"            <button class=\"btn-secondary\" onclick=\"sendCommand(102)\">Forward</button>\n"
"            <button class=\"btn-secondary\" onclick=\"sendCommand(104)\">Prev File</button>\n"
"            <button class=\"btn-secondary\" onclick=\"sendCommand(105)\">Next File</button>\n"
"            <button class=\"btn-secondary\" onclick=\"sendCommand(107)\">Speed Up</button>\n"
"            <button class=\"btn-secondary\" onclick=\"sendCommand(108)\">Normal Speed</button>\n"
"        </div>\n"
"\n"
"        <div class=\"info-panel\">\n"
"            <h3>System Log</h3>\n"
"            <div id=\"logContainer\"></div>\n"
"        </div>\n"
"    </div>\n"
"\n"
"    <script>\n"
"        let liveWs = null;\n"
"        let playbackWs = null;\n"
"        let videoDecoder = null;\n"
"        let audioDecoder = null;\n"
"        const canvas = document.getElementById('videoCanvas');\n"
"        const ctx = canvas.getContext('2d');\n"
"        let audioContext = null;\n"
"        let audioQueue = [];\n"
"        let nextAudioTime = 0;\n"
"        let videoFrameCount = 0;\n"
"        let audioFrameCount = 0;\n"
"        let lastStatsTime = Date.now();\n"
"        let receivedKeyFrame = false;\n"
"        let totalBytes = 0;\n"
"        let keyFrameTimeout = null;\n"
"        let decoderConfigured = false;\n"
"        let spsData = null;\n"
"        let ppsData = null;\n"
"        let pendingFrames = [];\n"
"\n"
"        function log(message) {\n"
"            const container = document.getElementById('logContainer');\n"
"            const entry = document.createElement('div');\n"
"            entry.className = 'log-entry';\n"
"            entry.textContent = '[' + new Date().toLocaleTimeString() + '] ' + message;\n"
"            container.insertBefore(entry, container.firstChild);\n"
"            while (container.children.length > 50) {\n"
"                container.removeChild(container.lastChild);\n"
"            }\n"
"        }\n"
"\n"
"        function updateStatus(message, connected) {\n"
"            const status = document.getElementById('status');\n"
"            status.textContent = message;\n"
"            status.className = 'status ' + (connected ? 'connected' : 'disconnected');\n"
"            document.getElementById('connectionStatus').textContent = connected ? 'Online' : 'Offline';\n"
"        }\n"
"\n"
"        function checkWebCodecsSupport() {\n"
"            log('Checking WebCodecs API support...');\n"
"            log('Browser: ' + navigator.userAgent);\n"
"            \n"
"            // 检查安全上下文\n"
"            const isSecureContext = window.isSecureContext;\n"
"            const protocol = location.protocol;\n"
"            const hostname = location.hostname;\n"
"            \n"
"            log('Protocol: ' + protocol);\n"
"            log('Hostname: ' + hostname);\n"
"            log('isSecureContext: ' + isSecureContext);\n"
"            \n"
"            // 使用typeof检测WebCodecs API构造函数\n"
"            const hasVideoDecoder = typeof window.VideoDecoder === 'function';\n"
"            const hasEncodedVideoChunk = typeof window.EncodedVideoChunk === 'function';\n"
"            const hasAudioDecoder = typeof window.AudioDecoder === 'function';\n"
"            const hasEncodedAudioChunk = typeof window.EncodedAudioChunk === 'function';\n"
"            const hasVideoFrame = typeof window.VideoFrame === 'function';\n"
"            \n"
"            log('typeof VideoDecoder: ' + typeof window.VideoDecoder);\n"
"            log('typeof EncodedVideoChunk: ' + typeof window.EncodedVideoChunk);\n"
"            log('typeof AudioDecoder: ' + typeof window.AudioDecoder);\n"
"            log('typeof EncodedAudioChunk: ' + typeof window.EncodedAudioChunk);\n"
"            log('typeof VideoFrame: ' + typeof window.VideoFrame);\n"
"            \n"
"            // 最终检测：VideoDecoder和EncodedVideoChunk必须都存在\n"
"            const webCodecsSupported = hasVideoDecoder && hasEncodedVideoChunk;\n"
"            \n"
"            log('WebCodecs API supported: ' + webCodecsSupported);\n"
"            \n"
"            if (webCodecsSupported) {\n"
"                log('WebCodecs API is supported!');\n"
"                return true;\n"
"            } else {\n"
"                log('ERROR: WebCodecs API is not supported');\n"
"                \n"
"                // 检查是否是安全上下文问题\n"
"                if (!isSecureContext && protocol === 'http:') {\n"
"                    log('=== SECURITY CONTEXT ERROR ===');\n"
"                    log('WebCodecs API requires a secure context (HTTPS or localhost)');\n"
"                    log('Current URL: ' + location.href);\n"
"                    log('');\n"
"                    log('SOLUTIONS:');\n"
"                    log('1. Use SSH port forwarding:');\n"
"                    log('   ssh -L 8080:localhost:8080 root@' + hostname);\n"
"                    log('   Then access: http://localhost:8080/');\n"
"                    log('');\n"
"                    log('2. Use Chrome/Edge with flag:');\n"
"                    log('   Add to shortcut target:');\n"
"                    log('   --unsafely-treat-insecure-origin-as-secure=http://' + hostname + ':8080');\n"
"                    log('');\n"
"                    log('3. Setup HTTPS (advanced)');\n"
"                } else {\n"
"                    log('Your browser: ' + navigator.userAgent);\n"
"                    log('Required: Chrome 94+, Edge 94+, or Safari 16.4+');\n"
"                }\n"
"                \n"
"                return false;\n"
"            }\n"
"        }\n"
"\n"
"        async function initVideoDecoder() {\n"
"            if (!checkWebCodecsSupport()) {\n"
"                alert('Your browser does not support WebCodecs API. Please use Chrome 94+, Edge 94+, or Safari 16.4+');\n"
"                return false;\n"
"            }\n"
"\n"
"            try {\n"
"                videoDecoder = new VideoDecoder({\n"
"                    output: function(videoFrame) {\n"
"                        try {\n"
"                            ctx.drawImage(videoFrame, 0, 0, canvas.width, canvas.height);\n"
"                            videoFrameCount++;\n"
"                        } catch (e) {\n"
"                            log('Render error: ' + e.message);\n"
"                        } finally {\n"
"                            videoFrame.close();\n"
"                        }\n"
"                    },\n"
"                    error: function(error) {\n"
"                        log('Video decode error: ' + error.message);\n"
"                        // 重置解码器状态，等待下一个关键帧重新初始化\n"
"                        videoDecoder = null;\n"
"                        receivedKeyFrame = false;\n"
"                        spsData = null;\n"
"                        ppsData = null;\n"
"                    }\n"
"                });\n"
"\n"
"                await videoDecoder.configure({\n"
"                    codec: 'avc1.42E01E',\n"
"                    codedWidth: 640,\n"
"                    codedHeight: 480\n"
"                });\n"
"\n"
"                log('Video decoder initialized successfully');\n"
"                return true;\n"
"            } catch (error) {\n"
"                log('Failed to initialize video decoder: ' + error.message);\n"
"                alert('Failed to initialize video decoder: ' + error.message);\n"
"                return false;\n"
"            }\n"
"        }\n"
"\n"
"        async function initAudioDecoder() {\n"
"            if (!('AudioDecoder' in window)) {\n"
"                log('Warning: Browser does not support audio decoding');\n"
"                return false;\n"
"            }\n"
"\n"
"            try {\n"
"                audioContext = new (window.AudioContext || window.webkitAudioContext)({\n"
"                    sampleRate: 48000\n"
"                });\n"
"\n"
"                audioDecoder = new AudioDecoder({\n"
"                    output: function(audioData) {\n"
"                        playAudio(audioData);\n"
"                        audioData.close();\n"
"                        audioFrameCount++\n"
"                    },\n"
"                    error: function(error) {\n"
"                        log('Audio decode error: ' + error.message);\n"
"                    }\n"
"                });\n"
"\n"
"                await audioDecoder.configure({\n"
"                    codec: 'opus',\n"
"                    sampleRate: 48000,\n"
"                    numberOfChannels: 2\n"
"                });\n"
"\n"
"                return true;\n"
"            } catch (error) {\n"
"                log('Failed to initialize audio decoder: ' + error.message);\n"
"                return false;\n"
"            }\n"
"        }\n"
"\n"
"        function playAudio(audioData) {\n"
"            try {\n"
"                const buffer = audioContext.createBuffer(\n"
"                    audioData.numberOfChannels,\n"
"                    audioData.numberOfFrames,\n"
"                    audioData.sampleRate\n"
"                );\n"
"\n"
"                for (let i = 0; i < audioData.numberOfChannels; i++) {\n"
"                    const channelData = buffer.getChannelData(i);\n"
"                    const options = { planeIndex: i, format: 'f32-planar' };\n"
"                    audioData.copyTo(channelData, options);\n"
"                }\n"
"\n"
"                const source = audioContext.createBufferSource();\n"
"                source.buffer = buffer;\n"
"                source.connect(audioContext.destination);\n"
"\n"
"                const currentTime = audioContext.currentTime;\n"
"                if (nextAudioTime < currentTime) {\n"
"                    nextAudioTime = currentTime;\n"
"                }\n"
"\n"
"                source.start(nextAudioTime);\n"
"                nextAudioTime += buffer.duration;\n"
"            } catch (error) {\n"
"                log('Audio playback error: ' + error.message);\n"
"            }\n"
"        }\n"
"\n"
"        function parsePacket(data) {\n"
"            if (data.length < 5) {\n"
"                log('Invalid packet: data length < 5');\n"
"                return null;\n"
"            }\n"
"            const view = new DataView(data.buffer);\n"
"            const length = view.getUint32(0);\n"
"            const flag = data[4];\n"
"            const payload = data.slice(5);\n"
"            return { length: length, flag: flag, payload: payload };\n"
"        }\n"
"\n"
"        function getNALType(h264Data) {\n"
"            let offset = 0;\n"
"            if (h264Data[0] === 0 && h264Data[1] === 0 && h264Data[2] === 0 && h264Data[3] === 1) {\n"
"                offset = 4;\n"
"            } else if (h264Data[0] === 0 && h264Data[1] === 0 && h264Data[2] === 1) {\n"
"                offset = 3;\n"
"            }\n"
"            if (offset >= h264Data.length) return -1;\n"
"            return h264Data[offset] & 0x1F;\n"
"        }\n"
"\n"
"        function isKeyFrame(h264Data) {\n"
"            const nalType = getNALType(h264Data);\n"
"            // 关键帧类型：5=I帧, 7=SPS, 8=PPS\n"
"            const result = nalType === 5 || nalType === 7 || nalType === 8;\n"
"            if (result) {\n"
"                log('Detected key frame (NAL type: ' + nalType + ')');\n"
"            }\n"
"            return result;\n"
"        }\n"
"\n"
"        function isIFrame(h264Data) {\n"
"            const nalType = getNALType(h264Data);\n"
"            // I帧：5\n"
"            return nalType === 5;\n"
"        }\n"
"\n"
"        function isSPSPPS(h264Data) {\n"
"            const nalType = getNALType(h264Data);\n"
"            // SPS: 7, PPS: 8\n"
"            return nalType === 7 || nalType === 8;\n"
"        }\n"
"\n"
"        function extractSPSPPS(data) {\n"
"            let sps = null;\n"
"            let pps = null;\n"
"            let offset = 0;\n"
"\n"
"            while (offset < data.length) {\n"
"                let startCodeLen = 0;\n"
"                if (offset + 4 <= data.length && data[offset] === 0 && data[offset+1] === 0 && data[offset+2] === 0 && data[offset+3] === 1) {\n"
"                    startCodeLen = 4;\n"
"                } else if (offset + 3 <= data.length && data[offset] === 0 && data[offset+1] === 0 && data[offset+2] === 1) {\n"
"                    startCodeLen = 3;\n"
"                }\n"
"\n"
"                if (startCodeLen === 0) {\n"
"                    break;\n"
"                }\n"
"\n"
"                const nalStart = offset + startCodeLen;\n"
"                if (nalStart >= data.length) break;\n"
"\n"
"                const nalType = data[nalStart] & 0x1F;\n"
"\n"
"                // 查找下一个起始码\n"
"                let nextStart = data.length;\n"
"                for (let i = nalStart + 1; i < data.length - 3; i++) {\n"
"                    if (data[i] === 0 && data[i+1] === 0 && (data[i+2] === 1 || (data[i+2] === 0 && data[i+3] === 1))) {\n"
"                        nextStart = i;\n"
"                        break;\n"
"                    }\n"
"                }\n"
"\n"
"                // 提取NAL数据（包含起始码）\n"
"                const nalData = data.slice(offset, nextStart);\n"
"\n"
"                if (nalType === 7) {\n"
"                    sps = nalData;\n"
"                } else if (nalType === 8) {\n"
"                    pps = nalData;\n"
"                }\n"
"\n"
"                offset = nextStart;\n"
"            }\n"
"\n"
"            return { sps: sps, pps: pps };\n"
"        }\n"
"\n"
"        async function processPendingFrames() {\n"
"            if (!spsData || !ppsData) {\n"
"                return;\n"
"            }\n"
"            if (pendingFrames.length === 0) {\n"
"                return;\n"
"            }\n"
"            log('Processing ' + pendingFrames.length + ' pending frames');\n"
"            for (let frame of pendingFrames) {\n"
"                await decodeFrame(frame.payload, frame.isI);\n"
"            }\n"
"            pendingFrames = [];\n"
"        }\n"
"\n"
"        async function decodeFrame(payload, isI) {\n"
"            // 检查解码器状态，如果不存在或已关闭则重新初始化\n"
"            if (!videoDecoder || videoDecoder.state === 'closed') {\n"
"                if (!isI) {\n"
"                    log('Decoder not ready, skipping P frame');\n"
"                    return;\n"
"                }\n"
"                log('Reinitializing video decoder');\n"
"                const success = await initVideoDecoder();\n"
"                if (!success) {\n"
"                    log('Failed to reinitialize decoder');\n"
"                    return;\n"
"                }\n"
"            }\n"
"\n"
"            if (isI) {\n"
"                log('I frame received, setting receivedKeyFrame=true');\n"
"                receivedKeyFrame = true;\n"
"                // 清除超时\n"
"                if (keyFrameTimeout) {\n"
"                    clearTimeout(keyFrameTimeout);\n"
"                    keyFrameTimeout = null;\n"
"                }\n"
"            }\n"
"\n"
"            // 对于I帧，在数据前面附加SPS和PPS\n"
"            let frameData = payload;\n"
"            if (isI && spsData && ppsData) {\n"
"                // 构造带SPS/PPS的I帧数据\n"
"                // 格式: [SPS with start code] [PPS with start code] [I frame with start code]\n"
"                frameData = new Uint8Array(spsData.length + ppsData.length + payload.length);\n"
"                frameData.set(spsData, 0);\n"
"                frameData.set(ppsData, spsData.length);\n"
"                frameData.set(payload, spsData.length + ppsData.length);\n"
"                log('I frame with SPS/PPS, total size=' + frameData.length);\n"
"            }\n"
"\n"
"            const chunk = new EncodedVideoChunk({\n"
"                type: isI ? 'key' : 'delta',\n"
"                timestamp: performance.now() * 1000,\n"
"                data: frameData\n"
"            });\n"
"\n"
"            try {\n"
"                await videoDecoder.decode(chunk);\n"
"            } catch (e) {\n"
"                if (e.message !== \"Cannot call 'decode' on a closed codec.\") {\n"
"                    log('Video decode failed: ' + e.message);\n"
"                }\n"
"            }\n"
"        }\n"
"\n"
"        async function startLive() {\n"
"            const serverIp = document.getElementById('serverIp').value;\n"
"            if (!serverIp) {\n"
"                alert('Please enter device IP address');\n"
"                return;\n"
"            }\n"
"\n"
"            if (!videoDecoder) {\n"
"                const success = await initVideoDecoder();\n"
"                if (!success) return;\n"
"            }\n"
"\n"
"            if (!audioDecoder) {\n"
"                await initAudioDecoder();\n"
"            }\n"
"\n"
"            const wsUrl = 'ws://' + serverIp + ':56070';\n"
"            log('Connecting to live server: ' + wsUrl);\n"
"\n"
"            liveWs = new WebSocket(wsUrl);\n"
"            liveWs.binaryType = 'arraybuffer';\n"
"\n"
"            liveWs.onopen = function() {\n"
"                log('Live WebSocket connected');\n"
"                updateStatus('Live connected', true);\n"
"                receivedKeyFrame = false;\n"
"                decoderConfigured = false;\n"
"                spsData = null;\n"
"                ppsData = null;\n"
"                pendingFrames = [];\n"
"                // 请求关键帧\n"
"                liveWs.send(new Uint8Array([0xFF]));\n"
"                // 设置超时，如果10秒内没有收到关键帧，就停止等待\n"
"                keyFrameTimeout = setTimeout(function() {\n"
"                    if (!receivedKeyFrame) {\n"
"                        log('Key frame timeout, accepting all frames');\n"
"                        receivedKeyFrame = true;\n"
"                    }\n"
"                }, 10000);\n"
"            }\n"
"\n"
"            liveWs.onmessage = async function(event) {\n"
"                try {\n"
"                    const data = new Uint8Array(event.data);\n"
"                    totalBytes += data.length;\n"
"                    const packet = parsePacket(data);\n"
"                    if (!packet) {\n"
"                        return;\n"
"                    }\n"
"\n"
"                    if (packet.flag === 0x80) {\n"
"                        const nalType = getNALType(packet.payload);\n"
"                        const isI = nalType === 5;\n"
"                        const isSPS = nalType === 7;\n"
"                        const isPPS = nalType === 8;\n"
"                        const isKey = isI || isSPS || isPPS;\n"
"\n"
"                        // 保存SPS/PPS数据（可能合在一起）\n"
"                        if (isSPS) {\n"
"                            // 尝试从数据中提取SPS和PPS\n"
"                            const extracted = extractSPSPPS(packet.payload);\n"
"                            if (extracted.sps) {\n"
"                                spsData = extracted.sps;\n"
"                                log('SPS data extracted, size=' + spsData.length);\n"
"                            }\n"
"                            if (extracted.pps) {\n"
"                                ppsData = extracted.pps;\n"
"                                log('PPS data extracted, size=' + ppsData.length);\n"
"                            }\n"
"                            // 如果有缓冲帧，尝试处理\n"
"                            await processPendingFrames();\n"
"                            return;\n"
"                        }\n"
"                        if (isPPS) {\n"
"                            ppsData = packet.payload.slice(0);\n"
"                            log('PPS data saved, size=' + ppsData.length);\n"
"                            // 如果有缓冲帧，尝试处理\n"
"                            await processPendingFrames();\n"
"                            return;\n"
"                        }\n"
"\n"
"                        // 在收到第一个关键帧之前，跳过P帧\n"
"                        if (!receivedKeyFrame && !isKey) {\n"
"                            log('Skipping P frame before key frame');\n"
"                            return;\n"
"                        }\n"
"\n"
"                        // 如果还没有SPS/PPS数据，缓冲帧\n"
"                        if (!spsData || !ppsData) {\n"
"                            log('Buffering frame, waiting for SPS/PPS');\n"
"                            pendingFrames.push({ payload: packet.payload, isI: isI });\n"
"                            return;\n"
"                        }\n"
"\n"
"                        // 处理帧\n"
"                        await decodeFrame(packet.payload, isI);\n"
"                    } else {\n"
"                        if (audioDecoder) {\n"
"                            const chunk = new EncodedAudioChunk({\n"
"                                type: 'key',\n"
"                                timestamp: performance.now() * 1000,\n"
"                                data: packet.payload\n"
"                            });\n"
"\n"
"                            try {\n"
"                                await audioDecoder.decode(chunk);\n"
"                            } catch (e) {\n"
"                                log('Audio decode failed: ' + e.message);\n"
"                            }\n"
"                        }\n"
"                    }\n"
"                } catch (error) {\n"
"                    log('Error processing WebSocket message: ' + error.message);\n"
"                }\n"
"            }\n"
"\n"
"            liveWs.onerror = function(error) {\n"
"                log('Live WebSocket error');\n"
"                updateStatus('Live connection error', false);\n"
"            }\n"
"\n"
"            liveWs.onclose = function() {\n"
"                log('Live WebSocket closed');\n"
"                updateStatus('Live disconnected', false);\n"
"                // 清除超时\n"
"                if (keyFrameTimeout) {\n"
"                    clearTimeout(keyFrameTimeout);\n"
"                    keyFrameTimeout = null;\n"
"                }\n"
"            }\n"
"        }\n"
"\n"
"        function stopLive() {\n"
"            if (liveWs) {\n"
"                liveWs.close();\n"
"                liveWs = null;\n"
"            }\n"
"            receivedKeyFrame = false;\n"
"            // 清除超时\n"
"            if (keyFrameTimeout) {\n"
"                clearTimeout(keyFrameTimeout);\n"
"                keyFrameTimeout = null;\n"
"            }\n"
"            updateStatus('Live stopped', false);\n"
"        }\n"
"\n"
"        async function startPlayback() {\n"
"            const serverIp = document.getElementById('serverIp').value;\n"
"            if (!serverIp) {\n"
"                alert('Please enter device IP address');\n"
"                return;\n"
"            }\n"
"\n"
"            if (!videoDecoder) {\n"
"                const success = await initVideoDecoder();\n"
"                if (!success) return;\n"
"            }\n"
"\n"
"            if (!audioDecoder) {\n"
"                await initAudioDecoder();\n"
"            }\n"
"\n"
"            const wsUrl = 'ws://' + serverIp + ':56080';\n"
"            log('Connecting to playback server: ' + wsUrl);\n"
"\n"
"            playbackWs = new WebSocket(wsUrl);\n"
"            playbackWs.binaryType = 'arraybuffer';\n"
"\n"
"            playbackWs.onopen = function() {\n"
"                log('Playback WebSocket connected');\n"
"                updateStatus('Playback connected', true);\n"
"                receivedKeyFrame = false;\n"
"                decoderConfigured = false;\n"
"                spsData = null;\n"
"                ppsData = null;\n"
"                pendingFrames = [];\n"
"                // 请求关键帧\n"
"                playbackWs.send(new Uint8Array([0xFF]));\n"
"                // 设置超时，如果10秒内没有收到关键帧，就停止等待\n"
"                keyFrameTimeout = setTimeout(function() {\n"
"                    if (!receivedKeyFrame) {\n"
"                        log('Key frame timeout, accepting all frames');\n"
"                        receivedKeyFrame = true;\n"
"                    }\n"
"                }, 10000);\n"
"            }\n"
"\n"
"            playbackWs.onmessage = async function(event) {\n"
"                try {\n"
"                    const data = new Uint8Array(event.data);\n"
"                    totalBytes += data.length;\n"
"                    const packet = parsePacket(data);\n"
"                    if (!packet) {\n"
"                        return;\n"
"                    }\n"
"\n"
"                    if (packet.flag === 0x80) {\n"
"                        const nalType = getNALType(packet.payload);\n"
"                        const isI = nalType === 5;\n"
"                        const isSPS = nalType === 7;\n"
"                        const isPPS = nalType === 8;\n"
"                        const isKey = isI || isSPS || isPPS;\n"
"\n"
"                        // 保存SPS/PPS数据（可能合在一起）\n"
"                        if (isSPS) {\n"
"                            // 尝试从数据中提取SPS和PPS\n"
"                            const extracted = extractSPSPPS(packet.payload);\n"
"                            if (extracted.sps) {\n"
"                                spsData = extracted.sps;\n"
"                                log('SPS data extracted, size=' + spsData.length);\n"
"                            }\n"
"                            if (extracted.pps) {\n"
"                                ppsData = extracted.pps;\n"
"                                log('PPS data extracted, size=' + ppsData.length);\n"
"                            }\n"
"                            // 如果有缓冲帧，尝试处理\n"
"                            await processPendingFrames();\n"
"                            return;\n"
"                        }\n"
"                        if (isPPS) {\n"
"                            ppsData = packet.payload.slice(0);\n"
"                            log('PPS data saved, size=' + ppsData.length);\n"
"                            // 如果有缓冲帧，尝试处理\n"
"                            await processPendingFrames();\n"
"                            return;\n"
"                        }\n"
"\n"
"                        // 在收到第一个关键帧之前，跳过P帧\n"
"                        if (!receivedKeyFrame && !isKey) {\n"
"                            log('Skipping P frame before key frame');\n"
"                            return;\n"
"                        }\n"
"\n"
"                        // 如果还没有SPS/PPS数据，缓冲帧\n"
"                        if (!spsData || !ppsData) {\n"
"                            log('Buffering frame, waiting for SPS/PPS');\n"
"                            pendingFrames.push({ payload: packet.payload, isI: isI });\n"
"                            return;\n"
"                        }\n"
"\n"
"                        // 处理帧\n"
"                        await decodeFrame(packet.payload, isI);\n"
"                    } else {\n"
"                        if (audioDecoder) {\n"
"                            const chunk = new EncodedAudioChunk({\n"
"                                type: 'key',\n"
"                                timestamp: performance.now() * 1000,\n"
"                                data: packet.payload\n"
"                        });\n"
"\n"
"                            try {\n"
"                                await audioDecoder.decode(chunk);\n"
"                            } catch (e) {\n"
"                                log('Audio decode failed: ' + e.message);\n"
"                            }\n"
"                        }\n"
"                    }\n"
"                } catch (error) {\n"
"                    log('Error processing WebSocket message: ' + error.message);\n"
"                }\n"
"            }\n"
"\n"
"            playbackWs.onerror = function(error) {\n"
"                log('Playback WebSocket error');\n"
"                updateStatus('Playback connection error', false);\n"
"            }\n"
"\n"
"            playbackWs.onclose = function() {\n"
"                log('Playback WebSocket closed');\n"
"                updateStatus('Playback disconnected', false);\n"
"                // 清除超时\n"
"                if (keyFrameTimeout) {\n"
"                    clearTimeout(keyFrameTimeout);\n"
"                    keyFrameTimeout = null;\n"
"                }\n"
"            }\n"
"        }\n"
"\n"
"        function stopPlayback() {\n"
"            if (playbackWs) {\n"
"                playbackWs.close();\n"
"                playbackWs = null;\n"
"            }\n"
"            receivedKeyFrame = false;\n"
"            // 清除超时\n"
"            if (keyFrameTimeout) {\n"
"                clearTimeout(keyFrameTimeout);\n"
"                keyFrameTimeout = null;\n"
"            }\n"
"            updateStatus('Playback stopped', false);\n"
"        }\n"
"\n"
"        function sendCommand(cmd) {\n"
"            if (!playbackWs || playbackWs.readyState !== WebSocket.OPEN) {\n"
"                log('Error: Playback not connected');\n"
"                return;\n"
"            }\n"
"\n"
"            const commandNames = {\n"
"                101: 'Rewind',\n"
"                102: 'Forward',\n"
"                104: 'Prev File',\n"
"                105: 'Next File',\n"
"                107: 'Speed Up',\n"
"                108: 'Normal Speed'\n"
"            }\n"
"\n"
"            playbackWs.send(new Uint8Array([cmd]));\n"
"            log('Send command: ' + (commandNames[cmd] || cmd));\n"
"        }\n"
"\n"
"        setInterval(function() {\n"
"            const now = Date.now();\n"
"            const elapsed = (now - lastStatsTime) / 1000;\n"
"\n"
"            document.getElementById('fps').textContent = Math.round(videoFrameCount / elapsed);\n"
"            document.getElementById('audioFps').textContent = Math.round(audioFrameCount / elapsed);\n"
"            document.getElementById('bitrate').textContent = Math.round((totalBytes * 8) / elapsed / 1024) + ' Kbps';\n"
"\n"
"            videoFrameCount = 0;\n"
"            audioFrameCount = 0;\n"
"            totalBytes = 0;\n"
"            lastStatsTime = now;\n"
"        }, 1000);\n"
"\n"
"        window.onload = function() {\n"
"            log('Page loaded');\n"
"            log('Browser: ' + navigator.userAgent);\n"
"            checkWebCodecsSupport();\n"
"        }\n"
"\n"
"        window.onbeforeunload = function() {\n"
"            stopLive();\n"
"            stopPlayback();\n"
"        }\n"
"    </script>\n"
"</body>\n"
"</html>\n";

C_HttpServer::C_HttpServer(int port)
    : m_port(port)
    , m_server_fd(-1)
    , m_bRunFlag(true)
{
}

C_HttpServer::~C_HttpServer()
{
    Stop();
}

int C_HttpServer::Start()
{
    // Create socket
    m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_fd < 0) {
        CLOG_ERR("HTTP socket creation failed: %s\n", strerror(errno));
        return -1;
    }

    // Set address reuse
    int opt = 1;
    if (setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        CLOG_ERR("HTTP setsockopt failed: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }

    // Set non-blocking mode
    int flags = fcntl(m_server_fd, F_GETFL, 0);
    fcntl(m_server_fd, F_SETFL, flags | O_NONBLOCK);

    // Bind address
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);

    if (bind(m_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        CLOG_ERR("HTTP bind failed: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }

    // Listen
    if (listen(m_server_fd, 10) < 0) {
        CLOG_ERR("HTTP listen failed: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }

    CLOG_INF("HTTP server started on port %d\n", m_port);
    CLOG_INF("Access URL: http://<device-ip>:%d\n", m_port);

    // Start accept thread
    m_acceptThread = std::thread(&C_HttpServer::AcceptThread, this);

    return 0;
}

void C_HttpServer::Stop()
{
    m_bRunFlag = false;

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (int fd : m_clientFds) {
            close(fd);
        }
        m_clientFds.clear();
    }

    if (m_server_fd >= 0) {
        close(m_server_fd);
    }

    CLOG_INF("HTTP server stopped\n");
}

void C_HttpServer::AcceptThread()
{
    fd_set read_fds;
    struct timeval tv;

    while (m_bRunFlag) {
        FD_ZERO(&read_fds);
        FD_SET(m_server_fd, &read_fds);

        // Add all clients to fd_set
        int max_fd = m_server_fd;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            for (int fd : m_clientFds) {
                FD_SET(fd, &read_fds);
                if (fd > max_fd) max_fd = fd;
            }
        }

        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms timeout

        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno != EINTR) {
                CLOG_ERR("HTTP select error: %s\n", strerror(errno));
            }
            continue;
        } else if (ret == 0) {
            continue;
        }

        // Handle new connection
        if (FD_ISSET(m_server_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(m_server_fd, (struct sockaddr *)&client_addr, &addr_len);

            if (new_fd >= 0) {
                // Set non-blocking
                int flags = fcntl(new_fd, F_GETFL, 0);
                fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);

                // Add to client set
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    m_clientFds.insert(new_fd);
                }

                CLOG_INF("HTTP new client connected: fd=%d\n", new_fd);

                // Start client processing thread
                std::thread clientThread(&C_HttpServer::ProcessClient, this, new_fd);
                clientThread.detach();
            }
        }
    }
}

void C_HttpServer::ProcessClient(int fd)
{
    std::vector<char> buffer(4096);
    std::string request;

    while (m_bRunFlag) {
        ssize_t n = recv(fd, buffer.data(), buffer.size(), 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            CLOG_ERR("HTTP recv error: %s\n", strerror(errno));
            break;
        } else if (n == 0) {
            // Client disconnected
            CLOG_INF("HTTP client disconnected: fd=%d\n", fd);
            break;
        }

        request.append(buffer.data(), n);

        // Check if complete HTTP request received
        if (request.find("\r\n\r\n") != std::string::npos) {
            // Handle HTTP request
            HandleHttpRequest(fd, request);
            break; // Close connection after handling request (HTTP 1.0)
        }
    }

    // Close connection
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clientFds.erase(fd);
    }
    close(fd);
}

void C_HttpServer::HandleHttpRequest(int fd, const std::string& request)
{
    // Parse request line
    size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
        SendHttpResponse(fd, 400, "Bad Request", "text/plain", "Invalid HTTP request");
        return;
    }

    std::string requestLine = request.substr(0, lineEnd);

    // Parse method and path
    size_t space1 = requestLine.find(' ');
    size_t space2 = requestLine.find(' ', space1 + 1);

    if (space1 == std::string::npos || space2 == std::string::npos) {
        SendHttpResponse(fd, 400, "Bad Request", "text/plain", "Invalid HTTP request");
        return;
    }

    std::string method = requestLine.substr(0, space1);
    std::string path = requestLine.substr(space1 + 1, space2 - space1 - 1);

    CLOG_INF("HTTP request: %s %s\n", method.c_str(), path.c_str());

    // Handle GET request
    if (method == "GET") {
        if (path == "/" || path == "/index.html") {
            // Return web_player.html
            SendHttpResponse(fd, 200, "OK", "text/html; charset=utf-8", WEB_PLAYER_HTML);
        } else {
            // 404 Not Found
            SendHttpResponse(fd, 404, "Not Found", "text/plain", "404 Not Found");
        }
    } else {
        // 405 Method Not Allowed
        SendHttpResponse(fd, 405, "Method Not Allowed", "text/plain", "Method Not Allowed");
    }
}

void C_HttpServer::SendHttpResponse(int fd, int statusCode, const std::string& statusText,
                                   const std::string& contentType, const std::string& content)
{
    std::string response = "HTTP/1.1 " + std::to_string(statusCode) + " " + statusText + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(content.length()) + "\r\n";
    response += "Connection: close\r\n";
    response += "Cache-Control: no-cache, no-store, must-revalidate\r\n";
    response += "Pragma: no-cache\r\n";
    response += "Expires: 0\r\n";
    response += "\r\n";
    response += content;

    send(fd, response.c_str(), response.length(), MSG_NOSIGNAL);
}

int C_HttpServer::GetClientCount()
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return m_clientFds.size();
}
