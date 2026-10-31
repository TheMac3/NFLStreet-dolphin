"""App-owned presentation only: figure-free bundle and one private PNG drop-in.

No disc writes, Reborn discovery, uploads, room identity or guest memory changes.
The generator and licence inventory live in tools/branding.py and docs/branding.md.
"""
from __future__ import annotations
import base64
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

TEXTURE = 'tex1_640x448_m_48996e5e01b5f7b8_3f49abd0c3a9b2bf_9.png'


def bundled() -> Path:
    """Resolve only shipped original graphics, independently of the player's cwd."""
    source=Path(__file__).resolve().parent
    base=Path(getattr(sys,'_MEIPASS',source if (source/'assets').is_dir() else source.parent))
    return base/'assets/branding/generated'


def direction() -> str:
    """The generated system file is the single packaged active-direction selector."""
    return json.loads((bundled()/'system.json').read_text())['active']


def artwork(folder: str) -> dict[str, Path]:
    """Use a validated private atlas when present, otherwise the original default.

    The 2160x1616 RGB/RGBA PNG is an already designed local drop-in, not a source
    photo to upload or redistribute. Crops stay beside it under local/. Replacing
    this file takes effect next UI open / launch, without rebuilding the program.
    """
    source=bundled()/direction()
    art={name:source/(name+'.png') for name in ('cover','loading','hero')}
    root=Path(folder).resolve();drop=root/'local/Branding/jsn.png'
    if not drop.is_file():return art
    if drop.is_symlink() or not drop.resolve().is_relative_to(root):return art
    try:
        from PIL import Image
        with Image.open(drop) as im:
            if im.format!='PNG' or im.size!=(2160,1616) or im.mode not in ('RGB','RGBA'):return art
            target=drop.parent/'rendered';_safe(root,target);target.mkdir(exist_ok=True)
            for name,box in {'cover':(0,0,880,1222),'loading':(880,0,2160,896),'hero':(880,896,2160,1616)}.items():
                out=target/(name+'.png');_safe(root,out);_safe(root,out.with_suffix('.tmp'))
                im.crop(box).convert('RGB').save(out.with_suffix('.tmp'),format='PNG');os.replace(out.with_suffix('.tmp'),out);art[name]=out
    except (OSError,ValueError):return {name:source/(name+'.png') for name in art}
    return art


def _safe(root: Path, path: Path) -> None:
    """App-owned outputs cannot follow symlinks or write another install's inode."""
    if (not path.resolve().is_relative_to(root)
            or any(p.is_symlink() for p in (path,*path.parents) if p.is_relative_to(root))
            or (path.is_file() and path.stat().st_nlink!=1)):
        raise ValueError('Branding output must stay in this app folder without links')


def overlay(folder: str, game_folder: str, enabled=True) -> str | None:
    """Create our last-precedence GX overlay; disabling removes it from the list.

    Only callers already enabling HiresTextures call this function. An untouched
    owned family therefore keeps its existing false HiresTextures decision.
    """
    if not enabled:return None
    root=Path(folder).resolve();dest=Path(game_folder).resolve()/'Branding/GN7E69'
    _safe(root,dest);dest.mkdir(parents=True,exist_ok=True)
    out=dest/TEXTURE;_safe(root,out);_safe(root,out.with_suffix('.tmp'))
    shutil.copyfile(artwork(folder)['loading'],out.with_suffix('.tmp'));os.replace(out.with_suffix('.tmp'),out)
    return str(dest)


def custom_cover(folder: str, game_folder: str, enabled=True) -> None:
    """Own only the cover we wrote: never overwrite a player's custom cover.

    A hash receipt lets disable remove our previous file and reveal Dolphin's
    existing fallback. No opening.bnr or image bytes are ever touched.
    """
    root=Path(folder).resolve();dest=Path(game_folder).resolve()
    out=dest/'nsr-net.cover.png';receipt=dest/'nsr-brand-cover.json'
    if not enabled and not receipt.exists():return
    _safe(root,dest);dest.mkdir(parents=True,exist_ok=True)
    for path in (out,receipt,receipt.with_suffix('.tmp'),out.with_suffix('.tmp')):_safe(root,path)
    try:
        old=json.loads(receipt.read_text()) if receipt.is_file() else {}
    except (OSError,ValueError):
        return  # A damaged ownership receipt never authorizes replacing player art.
    if out.exists() and hashlib.sha256(out.read_bytes()).hexdigest()!=old.get('sha256'):return
    if not enabled:
        if out.exists():out.unlink()
        if receipt.exists():receipt.unlink()
        return
    data=artwork(folder)['cover'].read_bytes();tmp=out.with_suffix('.tmp');tmp.write_bytes(data);os.replace(tmp,out)
    receipt.with_suffix('.tmp').write_text(json.dumps({'sha256':hashlib.sha256(data).hexdigest()}));os.replace(receipt.with_suffix('.tmp'),receipt)


def decorate_html(html: str, folder: str) -> str:
    """Embed local images into the existing WebView HTML; no web server or upload."""
    source=bundled();key=direction();system=json.loads((source/'system.json').read_text());palette=system['directions'][key]
    for token,path in {'HERO':artwork(folder)['hero'],'MARK':source/key/'mark-32.png'}.items():
        html=html.replace('__NSRB_'+token+'__','data:image/png;base64,'+base64.b64encode(path.read_bytes()).decode())
    return html.replace('__NSRB_INK__',palette['paper'] if key=='redline' else palette['dark']).replace('__NSRB_ACCENT__',palette['accent']).replace('__NSRB_PAPER__',palette['paper']).replace('__NSRB_DARK__',palette['dark'])
