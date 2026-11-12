#!/usr/bin/env python3
"""本地 CMS 接口模拟服务，用于验证 tvbox-nx 的协议实现。

它模拟 TVBox 生态里最常见的 CMS JSON 站源（type=1）：
    GET /api.php/provide/vod?ac=list
    GET /api.php/provide/vod?ac=detail&t=<type_id>&pg=<page>
    GET /api.php/provide/vod?ac=detail&ids=<vod_id>
    GET /api.php/provide/vod?ac=detail&wd=<keyword>

用法：
    python mock_cms.py            # 监听 127.0.0.1:8899
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT = 8899

CATEGORIES = [
    {"type_id": 1, "type_name": "电影"},
    {"type_id": 2, "type_name": "电视剧"},
    {"type_id": 4, "type_name": "动漫"},
]

# 三条线路：一条直链 m3u8（NS 上可播），一条需要解析的页面（NS 上不可播），一条 mp4
PLAY_FROM = "直链$$$需解析$$$备用"
PLAY_URL = (
    "第01集$https://example.invalid/demo/ep01.m3u8#第02集$https://example.invalid/demo/ep02.m3u8"
    "$$$"
    "第01集$https://example.invalid/page/ep01.html#第02集$https://example.invalid/page/ep02.html"
    "$$$"
    "第01集$https://example.invalid/demo/ep01.mp4"
)

VODS = []
for i in range(1, 26):
    VODS.append({
        "vod_id": i,
        "vod_name": f"测试影片 {i:02d}",
        "vod_pic": f"https://example.invalid/pic/{i}.jpg",
        "vod_remarks": f"更新至第{i}集" if i % 3 == 0 else "HD",
        "vod_year": "2026",
        "vod_area": "测试地区",
        "vod_actor": "演员A,演员B",
        "vod_director": "导演C",
        "vod_content": "这是一条用于验证详情页解析的简介文本。",
        "type_id": CATEGORIES[i % len(CATEGORIES)]["type_id"],
        "vod_play_from": PLAY_FROM,
        "vod_play_url": PLAY_URL,
    })


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("[mock] " + fmt % args + "\n")

    def _send(self, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        q = parse_qs(parsed.query)
        ac = (q.get("ac") or ["list"])[0]

        if parsed.path != "/api.php/provide/vod":
            self.send_response(404)
            self.end_headers()
            return

        if ac == "list":
            self._send({
                "class": CATEGORIES,
                "list": [self._brief(v) for v in VODS[:10]],
            })
            return

        if ac == "detail":
            if "ids" in q:
                ids = {int(x) for x in q["ids"][0].split(",") if x.isdigit()}
                self._send({"list": [v for v in VODS if v["vod_id"] in ids]})
                return
            if "wd" in q:
                kw = q["wd"][0]
                hits = [v for v in VODS if kw in v["vod_name"]]
                self._send({"page": 1, "pagecount": 1, "total": len(hits),
                            "list": hits})
                return
            # 分类列表
            tid = int((q.get("t") or ["0"])[0] or 0)
            pg = int((q.get("pg") or ["1"])[0] or 1)
            rows = [v for v in VODS if tid == 0 or v["type_id"] == tid]
            self._send({
                "page": pg,
                "pagecount": 2,
                "limit": "20",
                "total": len(rows),
                "list": rows,
            })
            return

        self._send({"code": 0, "msg": "unsupported ac", "list": []})

    @staticmethod
    def _brief(v):
        return {k: v[k] for k in ("vod_id", "vod_name", "vod_remarks", "vod_pic")}


def main():
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"mock CMS on http://127.0.0.1:{PORT}/api.php/provide/vod", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
