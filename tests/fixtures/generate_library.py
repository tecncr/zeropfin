#!/usr/bin/env python3
"""Generate tests/fixtures/library.json: a synthetic Jellyfin library.

All titles and metadata are fictional. Field names and shapes follow the
Jellyfin 10.10+/12.x BaseItemDto, MediaSourceInfo and MediaStream schemas
(checked against https://api.jellyfin.org/openapi/jellyfin-openapi-stable.json,
version 12.4.0, and the 10.10.0 controllers). Run:

    python3 tests/fixtures/generate_library.py > tests/fixtures/library.json
"""
import json
import sys

TICKS = 10_000_000
BYTES_PER_MS = 10


def oid(prefix, n):
    return f"{prefix}{n:031x}"


def stream_video(index, codec="h264", profile="High", level=41, w=1920, h=1080, fps=23.976, depth=8,
                 pixfmt="yuv420p", interlaced=False):
    return {"Index": index, "Type": "Video", "Codec": codec, "Profile": profile, "Level": level,
            "Width": w, "Height": h, "RealFrameRate": fps, "AverageFrameRate": fps, "BitDepth": depth,
            "PixelFormat": pixfmt, "IsInterlaced": interlaced, "IsDefault": True, "IsExternal": False,
            "DisplayTitle": f"{h}p {codec.upper()}"}


def stream_audio(index, codec="aac", channels=2, lang="eng", title=None):
    return {"Index": index, "Type": "Audio", "Codec": codec, "Channels": channels, "Language": lang,
            "IsDefault": index == 1, "IsExternal": False,
            "DisplayTitle": title or f"{lang.capitalize()} - {codec.upper()} - {'Stereo' if channels == 2 else f'{channels}ch'}"}


def stream_sub(index, codec="subrip", lang="eng", text=True, external=False):
    return {"Index": index, "Type": "Subtitle", "Codec": codec, "Language": lang,
            "IsTextSubtitleStream": text, "IsExternal": external, "DisplayTitle": f"{lang.capitalize()} - {codec.upper()}"}


def source(sid, container, streams, bitrate=8_000_000, size=None, runtime_s=600,
           protocol="File", name=None):
    # Virtual file sizes are proportional to runtime (10 bytes per millisecond)
    # so the mock player can derive a consistent duration from Content-Range.
    if size is None:
        size = runtime_s * 1000 * BYTES_PER_MS
    return {"Id": sid, "Protocol": protocol, "Type": "Default", "Container": container, "Size": size,
            "Name": name or "Default", "IsRemote": protocol != "File", "RunTimeTicks": runtime_s * TICKS,
            "SupportsDirectPlay": True, "SupportsDirectStream": True, "SupportsTranscoding": True,
            "VideoType": "VideoFile", "Bitrate": bitrate, "MediaStreams": streams,
            "DefaultAudioStreamIndex": 1}


def movie(n, name, year, sources, overview="", resume_s=0, played=False, rating=None, genres=()):
    iid = oid("a", n)
    runtime = sources[0]["RunTimeTicks"]
    return {"Id": iid, "Name": name, "Type": "Movie", "IsFolder": False, "ProductionYear": year,
            "RunTimeTicks": runtime, "Overview": overview, "ParentId": oid("c", 1),
            "OfficialRating": rating, "CommunityRating": 7.1, "Genres": list(genres),
            "ImageTags": {"Primary": f"p{n:04x}"}, "BackdropImageTags": [f"b{n:04x}"],
            "UserData": {"PlaybackPositionTicks": resume_s * TICKS, "Played": played, "PlayCount": 1 if played else 0,
                         "PlayedPercentage": (resume_s * TICKS * 100.0 / runtime) if resume_s else None},
            "MediaSources": sources, "DateCreated": f"2026-0{1 + n % 9}-1{n % 10}T12:00:00Z"}


def ok_sources(n, runtime_s=600, size=None, container="mkv"):
    return [source(oid("f", n), container, [stream_video(0), stream_audio(1)], runtime_s=runtime_s, size=size)]


def build():
    items = []
    items.append(movie(1, "Harbor Lights", 2021,
                       [source(oid("f", 1), "mkv", [stream_video(0), stream_audio(1), stream_sub(2)],
                               runtime_s=5400)],
                       overview="A lighthouse keeper keeps the lamp burning through one long winter.",
                       genres=("Drama",), rating="PG"))
    items.append(movie(2, "Northern Line", 2019,
                       [source(oid("f", 2), "mov,mp4,m4a,3gp,3g2,mj2",
                               [stream_video(0, profile="Main", level=31, w=1280, h=720, fps=25), stream_audio(1)],
                               bitrate=3_000_000, runtime_s=4800)],
                       overview="The last train north, told in real time.", resume_s=600, genres=("Thriller",)))
    items.append(movie(3, "Quiet Engine", 2023,
                       [source(oid("f", 3), "mkv", [stream_video(0, codec="hevc", profile="Main 10", level=120,
                                                                 depth=10, pixfmt="yuv420p10le"), stream_audio(1)])],
                       overview="HEVC fixture."))
    items.append(movie(4, "Paper Moons", 2018,
                       [source(oid("f", 4), "mkv", [stream_video(0, profile="High 10", depth=10,
                                                                 pixfmt="yuv420p10le"), stream_audio(1)])],
                       overview="Hi10P fixture."))
    items.append(movie(5, "Copper Field", 2017,
                       [source(oid("f", 5), "mkv", [stream_video(0), stream_audio(1, codec="dts", channels=6),
                                                    stream_audio(2, codec="aac")])],
                       overview="First audio track is DTS."))
    items.append(movie(6, "Fast Water", 2022,
                       [source(oid("f", 6), "mp4", [stream_video(0, fps=59.94, level=42), stream_audio(1)])],
                       overview="1080p60 fixture."))
    items.append(movie(7, "Heavy Rain", 2020,
                       [source(oid("f", 7), "mkv", [stream_video(0), stream_audio(1)], bitrate=25_000_000)],
                       overview="High bitrate fixture."))
    items.append(movie(8, "Two Sources", 2016,
                       [source(oid("f", 80), "mkv", [stream_video(0, codec="hevc"), stream_audio(1)], name="4K HEVC"),
                        source(oid("f", 81), "mp4", [stream_video(0, w=1280, h=720, level=31), stream_audio(1)],
                               name="720p", bitrate=4_000_000)],
                       overview="Second version is playable."))
    items.append(movie(9, "Glass Garden", 2001,
                       [source(oid("f", 9), "avi", [stream_video(0, codec="mpeg4", profile="Advanced Simple Profile",
                                                                 level=5), stream_audio(1, codec="mp3")])],
                       overview="AVI/MPEG-4 Part 2 fixture."))
    for n in range(10, 41):
        items.append(movie(n, f"Filler Film {n:02d}", 2000 + n % 25, ok_sources(n),
                           overview="Pagination filler."))
    # A short clip so natural end-of-file can be tested quickly.
    items.append(movie(41, "Short Clip", 2024, ok_sources(41, runtime_s=4), overview="Four seconds long."))

    series_id = oid("b", 1)
    seasons = [
        {"Id": oid("d", 1), "Name": "Season 1", "Type": "Season", "IsFolder": True, "IndexNumber": 1,
         "SeriesId": series_id, "SeriesName": "Lantern Bay", "ParentId": series_id,
         "ImageTags": {"Primary": "s0001"}, "UserData": {"UnplayedItemCount": 2, "Played": False}},
        {"Id": oid("d", 2), "Name": "Season 2", "Type": "Season", "IsFolder": True, "IndexNumber": 2,
         "SeriesId": series_id, "SeriesName": "Lantern Bay", "ParentId": series_id,
         "ImageTags": {"Primary": "s0002"}, "UserData": {"UnplayedItemCount": 1, "Played": False}},
    ]
    episodes = []
    for i, (season, num, name, ok) in enumerate([(1, 1, "Arrival", True), (1, 2, "Low Tide", True),
                                                 (1, 3, "The Fog Bell", True), (2, 1, "New Keeper", False)]):
        n = 101 + i
        streams = [stream_video(0, w=1280, h=720, level=31), stream_audio(1)] if ok else \
                  [stream_video(0, codec="hevc"), stream_audio(1)]
        episodes.append({
            "Id": oid("e", n), "Name": name, "Type": "Episode", "IsFolder": False, "IndexNumber": num,
            "ParentIndexNumber": season, "SeriesId": series_id, "SeriesName": "Lantern Bay",
            "SeasonId": oid("d", season), "SeasonName": f"Season {season}", "ParentId": oid("d", season),
            "RunTimeTicks": 1500 * TICKS, "Overview": f"Episode {num} of season {season}.",
            "ImageTags": {"Primary": f"e{n:04x}"}, "SeriesPrimaryImageTag": "ser0001",
            "ParentBackdropItemId": series_id, "ParentBackdropImageTags": ["serbd01"],
            "UserData": {"Played": i == 0, "PlaybackPositionTicks": 0},
            "MediaSources": [source(oid("f", n), "mkv", streams, runtime_s=1500)],
        })
    series = {"Id": series_id, "Name": "Lantern Bay", "Type": "Series", "IsFolder": True, "ProductionYear": 2024,
              "ParentId": oid("c", 2), "Overview": "A small town, a big storm, a lighthouse.",
              "ImageTags": {"Primary": "ser0001"}, "BackdropImageTags": ["serbd01"], "ChildCount": 2,
              "UserData": {"UnplayedItemCount": 3, "Played": False}, "Genres": ["Mystery"]}
    views = [
        {"Id": oid("c", 1), "Name": "Movies", "Type": "CollectionFolder", "CollectionType": "movies",
         "IsFolder": True, "ImageTags": {"Primary": "v0001"}},
        {"Id": oid("c", 2), "Name": "Shows", "Type": "CollectionFolder", "CollectionType": "tvshows",
         "IsFolder": True, "ImageTags": {"Primary": "v0002"}},
        {"Id": oid("c", 3), "Name": "Music", "Type": "CollectionFolder", "CollectionType": "music",
         "IsFolder": True, "ImageTags": {}},
    ]
    return {
        "server": {"LocalAddress": "http://127.0.0.1", "ServerName": "Fixture Jellyfin", "Version": "10.10.7",
                   "ProductName": "Jellyfin Server", "OperatingSystem": "", "Id": oid("9", 1),
                   "StartupWizardCompleted": True},
        "user": {"Id": oid("7", 1), "Name": "demo", "Password": "demo-password"},
        "token": "0f1e2d3c4b5a69788796a5b4c3d2e1f0",
        "views": views,
        "items": items + [series] + seasons + episodes,
    }


if __name__ == "__main__":
    json.dump(build(), sys.stdout, indent=1, sort_keys=True)
    sys.stdout.write("\n")
