#!/usr/bin/env python3
"""TVBox 源可用性探针 —— 在把源塞进 Switch 客户端之前，先在 PC 上把协议跑通。

它做的事，正是 tvbox-nx 的 C++ 层要做的事：
  1. 读配置（TVBox 配置 JSON，或单条 api 地址）
  2. 对每个站源打 ac=list，拿分类
  3. 拿分类第一页，取第一条详情
  4. 可选：跑一次搜索
  5. 把详情里的播放串拆成「直链(可播)」和「需解析(NS 上不可播)」

用法：
    python probe.py --config config.json
    python probe.py --url "https://host/api.php/provide/vod"
    python probe.py --config config.json --search 测试 --json report.json
"""
import argparse
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor

UA = ("Mozilla/5.0 (Nintendo Switch; tvbox-nx) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Version/4.0 Safari/537.36")

# 能直接交给 mpv 的直链后缀
DIRECT_EXT = (".m3u8", ".mp4", ".mkv", ".flv", ".ts", ".mpd")


def http_get_json(url, timeout=8):
    req = urllib.request.Request(url, headers={"User-Agent": UA,
                                               "Accept": "application/json,*/*"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
        charset = resp.headers.get_content_charset() or "utf-8"
    text = raw.decode(charset, "replace").strip()
    # 有些源在 JSON 前面挂 BOM 或注释
    text = text.lstrip("\ufeff")
    return json.loads(text)


def join(base, params):
    sep = "&" if "?" in base else "?"
    return base + sep + urllib.parse.urlencode(params)


def is_cms_site(site):
    """判断这个站源是不是本客户端能处理的 CMS JSON 类型。"""
    t = str(site.get("type", ""))
    if t == "3":
        return False, "type=3（csp/spider，需 ART 或 JS 引擎）"
    if t == "0":
        return False, "type=0（XML 站源，本客户端未实现）"
    api = (site.get("api") or "").strip()
    if not api:
        return False, "api 为空"
    if not api.startswith(("http://", "https://")):
        return False, "api 不是 http(s)，可能是本地 csp 路径"
    if api.endswith(".jar") or api.endswith(".js") or api.endswith(".py"):
        return False, "spider 脚本源"
    return True, "CMS JSON"


def split_play(vod):
    """vod_play_from / vod_play_url -> [{from, episodes:[{name,url,direct}]}]"""
    froms = (vod.get("vod_play_from") or "").split("$$$")
    urls = (vod.get("vod_play_url") or "").split("$$$")
    out = []
    for i, grp in enumerate(urls):
        name = froms[i] if i < len(froms) else f"线路{i + 1}"
        eps = []
        for item in grp.split("#"):
            item = item.strip()
            if not item:
                continue
            if "$" in item:
                ep_name, ep_url = item.split("$", 1)
            else:
                ep_name, ep_url = item, item
            path = urllib.parse.urlparse(ep_url).path.lower()
            direct = ep_url.startswith(("http://", "https://")) and path.endswith(DIRECT_EXT)
            eps.append({"name": ep_name, "url": ep_url, "direct": direct})
        out.append({"from": name, "episodes": eps})
    return out


def probe_site(site, timeout, search_kw=None):
    name = site.get("name") or site.get("key") or "?"
    ok, why = is_cms_site(site)
    result = {"name": name, "key": site.get("key", ""), "type": site.get("type"),
              "api": site.get("api", ""), "usable": False, "reason": why}
    if not ok:
        return result

    api = site["api"].strip()
    t0 = time.time()
    try:
        listing = http_get_json(join(api, {"ac": "list"}), timeout)
        cats = listing.get("class") or []
        result["latency_ms"] = int((time.time() - t0) * 1000)
        result["categories"] = [{"id": c.get("type_id"), "name": c.get("type_name")}
                                for c in cats][:12]

        detail = None
        sid = None
        if cats:
            sid = cats[0].get("type_id")
            page = http_get_json(join(api, {"ac": "detail", "t": sid, "pg": 1}), timeout)
            rows = page.get("list") or []
            if rows:
                vid = rows[0].get("vod_id")
                d = http_get_json(join(api, {"ac": "detail", "ids": vid}), timeout)
                detail = (d.get("list") or [None])[0]
                result["sample"] = rows[0].get("vod_name")
                result["pagecount"] = page.get("pagecount")

        lines = []
        if detail:
            lines = split_play(detail)
            result["routes"] = [{
                "from": ln["from"],
                "episodes": len(ln["episodes"]),
                "direct": sum(1 for e in ln["episodes"] if e["direct"]),
            } for ln in lines]

        if search_kw:
            s = http_get_json(join(api, {"ac": "detail", "wd": search_kw}), timeout)
            result["search_hits"] = len(s.get("list") or [])

        playable = any(e["direct"] for ln in lines for e in ln["episodes"])
        result["has_direct_stream"] = playable
        result["usable"] = True
        if lines and not playable:
            result["reason"] = "接口可用，但播放串全是需解析的页面链接"
        elif not lines:
            result["reason"] = "接口可用，但样例详情没有播放串"
    except Exception as e:  # noqa: BLE001
        result["latency_ms"] = int((time.time() - t0) * 1000)
        result["error"] = f"{type(e).__name__}: {e}"
    return result


def load_sites(cfg):
    if isinstance(cfg, dict) and "sites" in cfg:
        return cfg.get("sites") or []
    if isinstance(cfg, list):
        return cfg
    if isinstance(cfg, str):
        return [{"key": "manual", "name": "手填地址", "type": 1, "api": cfg}]
    raise SystemExit("配置格式不认识：需要 TVBox 配置(含 sites) 或单个 api 地址")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", help="TVBox 配置 JSON 文件")
    ap.add_argument("--url", help="单条 api 地址")
    ap.add_argument("--search", help="额外测试一次搜索关键词")
    ap.add_argument("--timeout", type=float, default=8)
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--json", help="把完整报告写到文件")
    args = ap.parse_args()

    if not args.config and not args.url:
        ap.error("至少给 --config 或 --url")

    if args.config:
        with open(args.config, "r", encoding="utf-8") as f:
            sites = load_sites(json.load(f))
    else:
        sites = load_sites(args.url)

    print(f"[*] 站源 {len(sites)} 个，并发 {args.workers}，超时 {args.timeout}s\n")
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        results = list(ex.map(lambda s: probe_site(s, args.timeout, args.search), sites))

    usable = [r for r in results if r.get("usable")]
    playable = [r for r in usable if r.get("has_direct_stream")]

    print(f"{'状态':<6}{'名称':<18}{'耗时':<8}说明")
    print("-" * 78)
    for r in sorted(results, key=lambda x: (not x.get("usable"), x["name"])):
        lat = f"{r['latency_ms']}ms" if "latency_ms" in r else "-"
        if r.get("usable"):
            status = "可播" if r.get("has_direct_stream") else "仅接口"
        else:
            status = "跳过" if not r.get("error") else "失败"
        note = r.get("reason", "")
        if r.get("error"):
            note = r["error"][:48]
        print(f"{status:<6}{r['name'][:16]:<18}{lat:<8}{note}")

    print(f"\n[=] 接口可用 {len(usable)} / {len(results)}，其中含直链 {len(playable)} 个")

    if playable:
        r = playable[0]
        print(f"\n[样例] {r['name']}  分类: {[c['name'] for c in r.get('categories', [])][:6]}")
        for rt in r.get("routes", []):
            print(f"   线路 {rt['from']}: 共 {rt['episodes']} 集，直链 {rt['direct']} 集")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(results, f, ensure_ascii=False, indent=2)
        print(f"\n[+] 报告已写入 {args.json}")

    return 0 if playable else 1


if __name__ == "__main__":
    sys.exit(main())
