"""Stage matching Retail DB2s and verified DBD schemas; preserve rollback copies."""
import datetime
import hashlib
import json
import pathlib
import shutil
import struct

root = pathlib.Path(__file__).resolve().parents[1]
project = pathlib.Path(r'D:\Server and Map Tools\Retail')
client = pathlib.Path(r'I:\WOW\Game\World of Warcraft')
definitions = pathlib.Path(r'I:\WOW\Server\Tools\DBC\Barebones-DBC-DB2-Editor-0.2.0-windows-x64\assets\definitions')
backup = pathlib.Path(r'I:\WOW\Server\Codex\Backups') / ('NoggitFix-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
backup.mkdir(parents=True)
tables = '''AreaTable AreaTrigger Map LoadingScreens Light LightData LightParams LightSkybox
GroundEffectDoodad GroundEffectTexture TerrainType LiquidType LiquidMaterial SoundProviderPreferences
SoundAmbience ZoneMusic ZoneIntroMusicTable WMOAreaTable SoundKit WindSettings Weather
WeatherXParticulate ZoneLight ZoneLightPoint MapDifficulty'''.split()
schema_paths = {p.stem.lower(): p for p in definitions.glob('*.dbd')}
plan = []
for name in tables:
    source = client / 'files/shadow-world/dbfilesclient' / (name + '.db2')
    if not source.exists():
        source = client / 'dbc/enUS' / (name + '.db2')
    with source.open('rb') as f:
        header = f.read(160)
    assert header[:4] == b'WDC5', source
    layout = f'{struct.unpack_from("<I", header, 156)[0]:08X}'
    schema = schema_paths[name.lower()]
    assert layout in schema.read_text(), (name, layout)
    plan += [(source, project / 'dbfilesclient' / (name + '.db2'), 'project/' + name + '.db2')]
    plan += [(schema, root / 'dist/definitions' / (name + '.dbd'), 'definitions/' + name + '.dbd')]

report = []
for source, target, relative in plan:
    saved = backup / relative
    saved.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        shutil.copy2(target, saved)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    assert digest(source) == digest(target)
    report.append(dict(source=str(source), target=str(target), backup=str(saved), sha256=digest(target)))
(backup / 'installation.json').write_text(json.dumps(report, indent=2))
print(f'Staged {len(tables)} DB2s and exact-layout schemas. Backup: {backup}')
