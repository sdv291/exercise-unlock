#!/usr/bin/env python3
"""Build the Exercise Unlock promo video.

    python build.py                  # English  -> out/ExerciseUnlock-promo.mp4 (+ .srt)
    python build.py --lang ru        # Russian  -> out/ExerciseUnlock-promo-ru.mp4 (+ .srt)
    python build.py --stills 3,25.5  # only PNG stills at those seconds -> out/stills/

Pipeline: narration (edge-tts, female neural voice) -> per-scene timeline
with word timings -> scene.html renders each frame in headless Chrome ->
ffmpeg muxes frames + voice/music/sfx mix + soft subtitles.

Requires: pip install -r requirements.txt  (plus Google Chrome installed).
"""
import argparse
import asyncio
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import wave

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "out"
CACHE = OUT / "cache"
RATE = "+0%"
FPS = 30
SR = 48000
W, H = 1920, 1080

# Per language: voice, narration per scene (id must match SCENES in
# scene.html; lead/tail = silence around the line) and marks — named
# moments the animation syncs to, each given as the start of the word
# that says it. `say` swaps words the voice should pronounce differently;
# subtitles keep the written form.
LANGS = {
    "en": dict(voice="en-US-AvaNeural", suffix="", say={}, scenes=[
        dict(id="intro", lead=0.9, tail=0.8, min=6.0, marks={},
             text="Meet Exercise Unlock: the computer that asks your kids to move before they play."),
        dict(id="sitting", lead=0.4, tail=0.8, min=0, marks=dict(posture="posture", energy="energy", focus="focus"),
             text="Kids can sit in front of a screen for hours without even noticing. "
                  "And long stretches of sitting are hard on growing bodies: on posture, on energy, and on focus."),
        dict(id="how", lead=0.4, tail=1.4, min=0, marks=dict(password="password", squats="squats"),
             text="Exercise Unlock turns the Windows lock screen into a friendly fitness coach. "
                  "Instead of typing a password, your child unlocks the computer with squats and push-ups."),
        dict(id="ai", lead=0.4, tail=1.6, min=0, marks=dict(ai="ai", half="half"),
             text="A regular webcam and on-device AI follow every movement and count only real, full reps. "
                  "Half-hearted bends simply don't count."),
        dict(id="earn", lead=0.4, tail=0.8, min=11.6, marks={},
             text="Every rep earns screen time. A few squats or push-ups, and the session is ready, "
                  "up to one full hour."),
        dict(id="hour", lead=0.4, tail=1.6, min=0,
             marks=dict(sixty="sixty", five="five", locks="locks", back="exercise", guar="guaranteed"),
             text="And here is the best part. A session never lasts longer than sixty minutes. "
                  "Five minutes before the end, a friendly reminder pops up. "
                  "Then the computer locks, and the only way back is another round of exercise. "
                  "That means a guaranteed movement break, every single hour."),
        dict(id="health", lead=0.4, tail=1.2, min=0,
             marks=dict(who="world", muscles="muscles", mood="mood", focus="focus", sitting="sitting"),
             text="The World Health Organization recommends that children get an average of at least "
                  "sixty minutes of physical activity a day. Short, regular bursts of movement help build "
                  "strong muscles and bones, lift mood and energy, sharpen focus, and break up long hours of sitting."),
        dict(id="parents", lead=0.4, tail=1.2, min=0, marks=dict(locks="locks", extra="extra", resets="resets"),
             text="Parents stay in control. Set the allowed hours, and the computer locks right on time in the "
                  "evening, with one extra minute to save and close apps. Earned time resets every day, "
                  "so there's no hoarding."),
        dict(id="privacy", lead=0.4, tail=1.2, min=0, marks=dict(camera="camera", processing="processing", nothing="nothing"),
             text="And privacy is built in. The camera switches on only at the lock screen, "
                  "and all the processing stays on your PC. Nothing is sent to the cloud."),
        dict(id="outro", lead=0.5, tail=2.6, min=7.0, marks={},
             text="Exercise Unlock. More movement, healthier habits, one rep at a time."),
    ]),
    "ru": dict(voice="ru-RU-SvetlanaNeural", suffix="-ru",
               say={"Exercise Unlock": "Экзерсайз Анлок", "Windows": "Виндоус"}, scenes=[
        dict(id="intro", lead=0.9, tail=0.8, min=6.0, marks={},
             text="Знакомьтесь: Exercise Unlock — компьютер, который просит детей подвигаться, прежде чем играть."),
        dict(id="sitting", lead=0.4, tail=0.8, min=0, marks=dict(posture="осанка", energy="энерги", focus="внимани"),
             text="Дети могут часами сидеть перед экраном и даже не замечать этого. "
                  "А долгое сидение без движения вредит растущему организму: страдают осанка, энергия и внимание."),
        dict(id="how", lead=0.4, tail=1.4, min=0, marks=dict(password="парол", squats="приседани"),
             text="Exercise Unlock превращает экран блокировки Windows в дружелюбного фитнес-тренера. "
                  "Вместо пароля ребёнок открывает компьютер приседаниями и отжиманиями."),
        dict(id="ai", lead=0.4, tail=1.6, min=0, marks=dict(ai="искусствен", half="халтурн"),
             text="Обычная веб-камера и искусственный интеллект прямо на компьютере следят за каждым движением "
                  "и засчитывают только настоящие, полные повторения. Халтурные полуприседы просто не считаются."),
        dict(id="earn", lead=0.4, tail=0.8, min=11.6, marks={},
             text="Каждое повторение приносит минуты за экраном. Несколько приседаний или отжиманий — "
                  "и сессия готова, максимум на целый час."),
        dict(id="hour", lead=0.4, tail=1.6, min=0,
             marks=dict(sixty="шестидесяти", five="пять", locks="блокируется", back="упражнени", guar="гарантирован"),
             text="А теперь самое главное. Сессия никогда не длится дольше шестидесяти минут. "
                  "За пять минут до конца появляется дружеское напоминание. "
                  "Затем компьютер блокируется, и вернуться можно только через новую порцию упражнений. "
                  "Это значит — гарантированный перерыв на движение каждый час."),
        dict(id="health", lead=0.4, tail=1.2, min=0,
             marks=dict(who="всемирн", muscles="мышц", mood="настроени", focus="концентраци", sitting="сидени"),
             text="Всемирная организация здравоохранения рекомендует детям в среднем не меньше шестидесяти минут "
                  "физической активности в день. Короткие регулярные разминки помогают укрепить мышцы и кости, "
                  "поднимают настроение и энергию, улучшают концентрацию и прерывают долгие часы сидения."),
        dict(id="parents", lead=0.4, tail=1.2, min=0, marks=dict(locks="заблокируется", extra="одну", resets="обнуляется"),
             text="Родители всё контролируют. Задайте разрешённые часы — и вечером компьютер заблокируется точно "
                  "по расписанию, оставив одну минуту, чтобы сохранить работу и закрыть программы. "
                  "Заработанное время обнуляется каждый день, так что копить его не получится."),
        dict(id="privacy", lead=0.4, tail=1.2, min=0, marks=dict(camera="камера", processing="обработка", nothing="ничего"),
             text="И конечно, приватность. Камера включается только на экране блокировки, "
                  "а вся обработка происходит на вашем компьютере. Ничего не отправляется в облако."),
        dict(id="outro", lead=0.5, tail=2.6, min=7.0, marks={},
             text="Exercise Unlock. Больше движения, полезные привычки — повтор за повтором."),
    ]),
    "uk": dict(voice="uk-UA-PolinaNeural", suffix="-uk",
               say={"Exercise Unlock": "Екзерсайз Анлок", "Windows": "Віндоус"}, scenes=[
        dict(id="intro", lead=0.9, tail=0.8, min=6.0, marks={},
             text="Знайомтеся: Exercise Unlock — комп'ютер, який просить дітей порухатися, перш ніж грати."),
        dict(id="sitting", lead=0.4, tail=0.8, min=0, marks=dict(posture="постав", energy="енергі", focus="уваг"),
             text="Діти можуть годинами сидіти перед екраном і навіть цього не помічати. "
                  "А довге сидіння без руху шкодить організму, що росте: страждають постава, енергія та увага."),
        dict(id="how", lead=0.4, tail=1.4, min=0, marks=dict(password="парол", squats="присідан"),
             text="Exercise Unlock перетворює екран блокування Windows на дружнього фітнес-тренера. "
                  "Замість пароля дитина відкриває комп'ютер присіданнями та віджиманнями."),
        dict(id="ai", lead=0.4, tail=1.6, min=0, marks=dict(ai="штучн", half="халтурн"),
             text="Звичайна вебкамера та штучний інтелект прямо на комп'ютері стежать за кожним рухом "
                  "і зараховують лише справжні, повні повторення. Халтурні напівприсіди просто не рахуються."),
        dict(id="earn", lead=0.4, tail=0.8, min=11.6, marks={},
             text="Кожне повторення приносить хвилини за екраном. Кілька присідань або віджимань — "
                  "і сесія готова, максимум на цілу годину."),
        dict(id="hour", lead=0.4, tail=1.6, min=0,
             marks=dict(sixty="шістдесят", five="нагадуван", locks="блокується", back="вправ", guar="гарантован"),
             text="А тепер найголовніше. Сесія ніколи не триває довше шістдесяти хвилин. "
                  "За п'ять хвилин до кінця з'являється дружнє нагадування. "
                  "Потім комп'ютер блокується, і повернутися можна лише через нову порцію вправ. "
                  "Це означає — гарантована перерва на рух щогодини."),
        dict(id="health", lead=0.4, tail=1.2, min=0,
             marks=dict(who="всесвітн", muscles="зміцн", mood="настр", focus="концентрац", sitting="сидінн"),
             text="Всесвітня організація охорони здоров'я рекомендує дітям у середньому щонайменше шістдесят хвилин "
                  "фізичної активності на день. Короткі регулярні розминки допомагають зміцнити м'язи та кістки, "
                  "покращують настрій і додають енергії, підвищують концентрацію та переривають довгі години сидіння."),
        dict(id="parents", lead=0.4, tail=1.2, min=0, marks=dict(locks="заблокується", extra="одну", resets="обнуляється"),
             text="Батьки все контролюють. Задайте дозволені години — і ввечері комп'ютер заблокується точно "
                  "за розкладом, залишивши одну хвилину, щоб зберегти роботу й закрити програми. "
                  "Зароблений час обнуляється щодня, тож накопичувати його не вийде."),
        dict(id="privacy", lead=0.4, tail=1.2, min=0, marks=dict(camera="камера", processing="обробка", nothing="нічого"),
             text="І звісно, приватність. Камера вмикається лише на екрані блокування, "
                  "а вся обробка відбувається на вашому комп'ютері. Нічого не надсилається в хмару."),
        dict(id="outro", lead=0.5, tail=2.6, min=7.0, marks={},
             text="Exercise Unlock. Більше руху, корисні звички — повтор за повтором."),
    ]),
}


def ffmpeg_exe():
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


# ------------------------------------------------------------------ narration
def spoken(text, lang):
    for written, said in LANGS[lang]["say"].items():
        text = text.replace(written, said)
    return text


def written(text, lang):
    for w, said in LANGS[lang]["say"].items():
        text = text.replace(said, w)
    return text


async def tts_one(sc, lang):
    voice, say = LANGS[lang]["voice"], spoken(sc["text"], lang)
    key = hashlib.sha1(f"{voice}|{RATE}|{say}".encode()).hexdigest()[:12]
    mp3, meta = CACHE / f"{lang}-{sc['id']}-{key}.mp3", CACHE / f"{lang}-{sc['id']}-{key}.json"
    if mp3.exists() and meta.exists():
        return mp3, json.loads(meta.read_text(encoding="utf-8"))
    import edge_tts
    comm = edge_tts.Communicate(say, voice, rate=RATE, boundary="WordBoundary")
    audio, words = bytearray(), []
    async for ch in comm.stream():
        if ch["type"] == "audio":
            audio += ch["data"]
        elif ch["type"] == "WordBoundary":
            words.append({"w": ch["text"], "t": ch["offset"] / 1e7, "d": ch["duration"] / 1e7})
    mp3.write_bytes(audio)
    meta.write_text(json.dumps(words, ensure_ascii=False), encoding="utf-8")
    return mp3, words


def norm(w):
    return re.sub(r"[^\w-]", "", w.lower()).replace("ё", "е")


def find_marks(sc, words):
    out = {}
    for name, stem in sc["marks"].items():
        hit = next((w for w in words if norm(w["w"]).startswith(norm(stem))), None)
        if hit is None:
            print(f"  warning: mark '{name}' ('{stem}') not found in scene {sc['id']}")
            continue
        out[name] = round(sc["lead"] + hit["t"], 3)
    return out


def decode(path):
    raw = subprocess.run([ffmpeg_exe(), "-v", "error", "-i", str(path), "-f", "s16le", "-ac", "1",
                          "-ar", str(SR), "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.int16).astype(np.float32) / 32768.0


def build_timeline(lang):
    CACHE.mkdir(parents=True, exist_ok=True)
    scenes, voices, t = [], [], 0.0
    for sc in LANGS[lang]["scenes"]:
        mp3, words = asyncio.run(tts_one(sc, lang))
        pcm = decode(mp3)
        dur = max(sc["min"], sc["lead"] + len(pcm) / SR + sc["tail"])
        scenes.append({"id": sc["id"], "start": round(t, 3), "dur": round(dur, 3),
                       "marks": find_marks(sc, words)})
        voices.append((t + sc["lead"], pcm, words, sc))
        t += dur
    return {"scenes": scenes, "total": round(t, 3)}, voices


# ------------------------------------------------------------------ sound design
def env_exp(n, rate):
    return np.exp(-np.arange(n) / SR * rate)


def tone(freq, dur, rate=6.0, harm=(1.0,)):
    n = int(dur * SR)
    tt = np.arange(n) / SR
    sig = sum(a * np.sin(2 * np.pi * freq * (i + 1) * tt) for i, a in enumerate(harm))
    att = np.minimum(1, tt / 0.004)
    return sig * env_exp(n, rate) * att


def noise(dur, seed=1):
    return np.random.default_rng(seed).uniform(-1, 1, int(dur * SR))


def smooth(x, k):
    return np.convolve(x, np.ones(k) / k, mode="same")


def sfx(kind):
    if kind == "ding":
        out = 0.5 * tone(1318.5, 0.5, 7, (1, .25, .1))
        hi = 0.3 * tone(1975.5, 0.4, 9)
        out[:len(hi)] += hi
        return out
    if kind == "pop":
        n = int(0.09 * SR)
        tt = np.arange(n) / SR
        f = np.linspace(380, 900, n)
        return 0.55 * np.sin(2 * np.pi * np.cumsum(f) / SR) * env_exp(n, 35) * np.minimum(1, tt / 0.003)
    if kind == "success":
        out = np.zeros(int(0.9 * SR))
        for i, fr in enumerate([523.25, 659.25, 783.99, 1046.5]):
            s = 0.35 * tone(fr, 0.6, 6, (1, .3))
            o = int(i * 0.08 * SR)
            out[o:o + len(s)] += s[:len(out) - o]
        return out
    if kind == "chime":
        a, b = 0.45 * tone(880, 0.7, 5, (1, .3)), 0.45 * tone(1318.5, 0.7, 5, (1, .3))
        out = np.zeros(int(0.95 * SR))
        out[:len(a)] += a
        o = int(0.16 * SR)
        out[o:o + len(b)] += b[:len(out) - o]
        return out
    if kind == "click":
        n = int(0.25 * SR)
        thump = tone(95, 0.25, 25) * 0.9
        clk = np.zeros(n)
        c = noise(0.02, 3) * env_exp(int(0.02 * SR), 200)
        clk[:len(c)] += c * 0.6
        return thump[:n] + clk
    if kind == "buzz":
        n = int(0.35 * SR)
        tt = np.arange(n) / SR
        return 0.35 * np.sign(np.sin(2 * np.pi * 140 * tt)) * smooth(np.ones(n), 400) * env_exp(n, 6) * 0.6
    if kind == "scratch":
        x = smooth(noise(0.3, 7), 6) * np.hanning(int(0.3 * SR))
        return 0.5 * x
    if kind == "whoosh":
        n = int(0.55 * SR)
        x = noise(0.55, 11)
        x = smooth(x, 24) - smooth(x, 200)
        return 1.6 * x * np.hanning(n)
    if kind == "sparkle":
        out = np.zeros(int(1.0 * SR))
        for i, fr in enumerate([1568, 2093, 2637, 3136, 2349]):
            s = 0.18 * tone(fr, 0.5, 9)
            o = int(i * 0.07 * SR)
            out[o:o + len(s)] += s[:len(out) - o]
        return out
    raise ValueError(kind)


def music(total):
    """Light, warm loop: I-V-vi-IV pads + plucked arpeggio + soft kick/hat."""
    n = int(total * SR)
    out = np.zeros(n)
    bpm = 100
    beat = 60 / bpm
    bar = 4 * beat
    prog = [[60, 64, 67], [55, 59, 62], [57, 60, 64], [53, 57, 60]]
    mtof = lambda m: 440 * 2 ** ((m - 69) / 12)
    nbars = int(total / bar) + 1
    for b in range(nbars):
        ch = prog[b % 4]
        s = int(b * bar * SR)
        if s >= n:
            break
        ln = min(n - s, int((bar + 0.6) * SR))
        tt = np.arange(ln) / SR
        env = np.minimum(1, tt / 0.6) * np.clip((bar + 0.6 - tt) / 0.8, 0, 1)
        pad = sum(np.sin(2 * np.pi * mtof(m) * tt) + 0.25 * np.sin(2 * np.pi * 2 * mtof(m) * tt + 0.3) for m in ch)
        pad += 0.7 * np.sin(2 * np.pi * mtof(ch[0] - 12) * tt)
        out[s:s + ln] += 0.035 * env * pad
        arp = [0, 1, 2, 1, 0, 2, 1, 2]
        for i in range(8):
            o = s + int(i * beat / 2 * SR)
            if o >= n:
                break
            note = ch[arp[i]] + 12 + (12 if i in (3, 7) else 0)
            pl = tone(mtof(note), 0.45, 9, (1, .45, .2))
            out[o:o + len(pl)] += 0.07 * pl[:n - o]
        for i in range(4):
            o = s + int(i * beat * SR)
            if o >= n:
                break
            if i in (0, 2):
                kn = int(0.18 * SR)
                tt2 = np.arange(kn) / SR
                fr = 48 + 70 * np.exp(-tt2 * 30)
                k = np.sin(2 * np.pi * np.cumsum(fr) / SR) * env_exp(kn, 16)
                out[o:o + kn] += 0.22 * k[:n - o]
            ho = o + int(beat / 2 * SR)
            if ho < n:
                hn = int(0.05 * SR)
                hh = noise(0.05, b * 8 + i)
                hh = hh - smooth(hh, 5)
                out[ho:ho + hn] += 0.05 * (hh * env_exp(hn, 70))[:n - ho]
    fade_in, fade_out = int(1.0 * SR), int(2.5 * SR)
    out[:fade_in] *= np.linspace(0, 1, fade_in)
    out[-fade_out:] *= np.linspace(1, 0, fade_out)
    return out / (np.max(np.abs(out)) + 1e-9)


def mix_audio(tl, voices, events, path, with_music=True):
    n = int(tl["total"] * SR) + SR
    voice, fx = np.zeros(n), np.zeros(n)
    for at, pcm, _, _ in voices:
        o = int(at * SR)
        voice[o:o + len(pcm)] += pcm[:n - o]
    voice *= 0.92 / (np.max(np.abs(voice)) + 1e-9)
    for t, kind in events:
        s = sfx(kind)
        o = max(0, int(t * SR))
        fx[o:o + len(s)] += s[:n - o]
    out = voice + 0.28 * fx
    if with_music:
        m = music(tl["total"] + 1)[:n]
        level = smooth(np.abs(voice), int(0.25 * SR))
        duck = 1 - 0.55 * np.clip(level / 0.06, 0, 1)
        out += 0.20 * m * smooth(duck, int(0.2 * SR))
    out = np.tanh(out * 1.05) / np.tanh(1.05)
    out *= 0.95 / (np.max(np.abs(out)) + 1e-9)
    pcm = (out * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


# ------------------------------------------------------------------ subtitles
def srt_time(t):
    ms = int(round(t * 1000))
    return f"{ms // 3600000:02}:{ms // 60000 % 60:02}:{ms // 1000 % 60:02},{ms % 1000:03}"


def build_srt(voices, path, lang):
    cues = []
    for at, _, words, sc in voices:
        text = spoken(sc["text"], lang)   # word timings refer to the spoken form
        low, cur, pos = text.lower(), 0, []
        for w in words:
            i = low.find(w["w"].lower(), cur)
            if i < 0:
                pos.append(cur)
                continue
            pos.append(i)
            cur = i + len(w["w"])
        # chunks: sentences, long ones split at commas
        chunks, start = [], 0
        for m in re.finditer(r"[.!?:](\s|$)|,\s", text):
            end = m.end()
            if text[m.start()] == "," and end - start < 45:
                continue
            chunks.append((start, end))
            start = end
        if start < len(text):
            chunks.append((start, len(text)))
        for a, b in chunks:
            idx = [k for k, p in enumerate(pos) if a <= p < b]
            if not idx:
                continue
            t0 = at + words[idx[0]]["t"]
            t1 = at + words[idx[-1]]["t"] + words[idx[-1]]["d"] + 0.25
            cues.append((t0, t1, written(text[a:b].strip(), lang)))
    # never overlap the next cue, or players stack two lines
    cues = [(t0, min(t1, cues[i + 1][0] - 0.05) if i + 1 < len(cues) else t1, s)
            for i, (t0, t1, s) in enumerate(cues)]
    lines = []
    for i, (t0, t1, s) in enumerate(cues, 1):
        lines += [str(i), f"{srt_time(t0)} --> {srt_time(t1)}", s, ""]
    path.write_text("\n".join(lines), encoding="utf-8")


# ------------------------------------------------------------------ frames
def open_page(p, tl, lang):
    browser = p.chromium.launch(channel="chrome", headless=True)
    page = browser.new_page(viewport={"width": W, "height": H}, device_scale_factor=1)
    page.goto((HERE / "scene.html").as_uri() + f"?render&lang={lang}")
    page.evaluate("l => setLang(l)", lang)
    page.evaluate("tl => setTimeline(tl)", tl)
    ok = page.evaluate("""async () => {
        render(1.5);
        await Promise.all(['700 50px Fredoka','600 50px Fredoka','800 50px Nunito','700 50px Nunito','900 50px Nunito']
            .map(f => document.fonts.load(f)));
        await document.fonts.ready;
        return [document.fonts.check('700 50px Fredoka'), document.fonts.check('900 50px Nunito')];
    }""")
    print("fonts loaded:", ok)
    return browser, page


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stills", help="comma-separated seconds; render PNGs only")
    ap.add_argument("--no-music", action="store_true")
    ap.add_argument("--lang", default="en", choices=sorted(LANGS))
    args = ap.parse_args()

    from playwright.sync_api import sync_playwright

    lang, suffix = args.lang, LANGS[args.lang]["suffix"]
    tl, voices = build_timeline(lang)
    (OUT / f"timeline{suffix}.json").write_text(json.dumps(tl, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"timeline: {len(tl['scenes'])} scenes, {tl['total']:.1f} s")
    for s in tl["scenes"]:
        print(f"  {s['id']:<8} start={s['start']:6.2f} dur={s['dur']:5.2f}")

    with sync_playwright() as p:
        browser, page = open_page(p, tl, lang)
        if args.stills:
            d = OUT / f"stills{suffix}"
            d.mkdir(parents=True, exist_ok=True)
            for s in args.stills.split(","):
                t = float(s)
                page.evaluate("t => render(t)", t)
                page.screenshot(path=str(d / f"t{t:06.2f}.png"))
                print("still", t)
            browser.close()
            return

        events = page.evaluate("() => collectSfx()")
        wav, srt = CACHE / f"mix{suffix}.wav", OUT / f"ExerciseUnlock-promo{suffix}.srt"
        mix_audio(tl, voices, events, wav, with_music=not args.no_music)
        build_srt(voices, srt, lang)

        mp4 = OUT / f"ExerciseUnlock-promo{suffix}.mp4"
        nframes = int(round(tl["total"] * FPS))
        cmd = [ffmpeg_exe(), "-y", "-v", "error",
               "-f", "image2pipe", "-framerate", str(FPS), "-c:v", "mjpeg", "-i", "-",
               "-i", str(wav), "-i", str(srt),
               "-map", "0:v", "-map", "1:a", "-map", "2:s",
               # JPEG frames are full-range; convert to standard TV range for players.
               "-vf", "scale=in_range=pc:out_range=tv,format=yuv420p", "-color_range", "tv",
               "-c:v", "libx264", "-preset", "medium", "-crf", "18",
               "-c:a", "aac", "-b:a", "192k", "-ac", "2",
               "-c:s", "mov_text", "-metadata:s:s:0", f"language={dict(en='eng', ru='rus', uk='ukr')[lang]}",
               "-movflags", "+faststart", "-t", f"{tl['total']:.3f}", str(mp4)]
        enc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        for i in range(nframes):
            page.evaluate("t => render(t)", i / FPS)
            enc.stdin.write(page.screenshot(type="jpeg", quality=93))
            if i % 150 == 0:
                print(f"frame {i}/{nframes}", flush=True)
        enc.stdin.close()
        rc = enc.wait()
        browser.close()
        if rc != 0:
            sys.exit(f"ffmpeg failed ({rc})")
        print("done:", mp4)


if __name__ == "__main__":
    main()
