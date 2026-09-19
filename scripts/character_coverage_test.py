#!/usr/bin/env python3
"""Bounded device display tests; software injection, NOT microphone/visual proof."""
import argparse
import json
import re
import time
from pathlib import Path
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--music', action='store_true', help='Play audible official test audio twice')
    parser.add_argument('--reset', action='store_true', help='Restart device and verify startup frames before tests')
    parser.add_argument('--music-tail', action='store_true', help='Only test cover fallback/recovery and one natural audio completion (up to 240s)')
    args = parser.parse_args()
    args.music = args.music or args.music_tail
    results = []
    with args.output.open('x') as out, serial.Serial(args.port, 115200, timeout=.2, dsrdtr=True, rtscts=True) as conn:
        def record(item):
            out.write(json.dumps(item, ensure_ascii=False)+'\n'); out.flush()

        def send(command):
            record({'send': command})
            conn.write(('character-test '+command+'\n').encode()); conn.flush()

        def collect(seconds):
            lines = []
            until = time.monotonic()+seconds
            while time.monotonic()<until:
                line=conn.readline().decode('utf-8','replace').strip()
                if not line: continue
                record({'log':line}); lines.append(line)
                if any(x in line for x in ('Guru Meditation','Stack canary','Brownout','watchdog','Character live failed')):
                    raise RuntimeError(line)
            return '\n'.join(lines)

        def check(name, command, seconds, patterns):
            send(command)
            log=collect(seconds)
            for pattern in patterns:
                if not re.search(pattern,log): raise AssertionError(name+': missing '+pattern)
            results.append(name); record({'test':name,'result':'PASS'})
            print('PASS: '+name,flush=True)
            return log

        try:
            conn.reset_input_buffer()
            if args.reset:
                from esptool.reset import HardReset
                conn.setDTR(False)
                HardReset(conn,uses_usb=True)()
                boot=collect(15)
                assert 'Character frame pose=12 submitted=1' in boot, 'startup frame missing'
                assert 'Character frame pose=0 submitted=1' in boot, 'idle frame missing'
                record({'test':'boot','result':'PASS'}); results.append('boot')
                print('PASS: boot',flush=True)
            send('cancel'); collect(2)
            if args.music_tail:
                check('music-start','music-play',12,[r'probe music accepted=1',r'Companion foreground running=1'])
                check('music-cover-fallback','fallback',3,[r'overlay visible=1',r'probe fallback=1'])
                check('music-cat-recovery','recover',3,[r'Companion foreground running=1'])
                until=time.monotonic()+240
                log=''
                while time.monotonic()<until:
                    log+=collect(1)
                    if 'audio_complete' in log: break
                assert re.search(r'audio_complete .*underruns=0 underrun_total_ms=0',log), 'natural completion missing/underrun'
                log+=collect(7)
                assert 'Character frame pose=0 submitted=1' in log, 'idle not restored'
                assert 'Immediate music stop requested active=1' not in log, 'not a natural end'
                record({'test':'music-natural-end','result':'PASS'})
                record({'suite':'PASS','tests':4,'limitations':['software injection, not ASR','no physical screen capture','single-track regression, not long-duration stability']})
                print('PASS: 4 music lifecycle tests',flush=True)
                return
            states={'startup':12,'connecting':14,'wake':1,'listen':13,'think':14,'tool':14,'speak':15,'success':16,'error':19,'fatal':21}
            for name,pose in states.items():
                check('state-'+name,'state '+name,3,[rf'probe state={name} accepted=1',rf'Character frame pose={pose} submitted=1'])
            emotions={'happy':16,'laughing':16,'funny':16,'loving':16,'confident':16,'delicious':16,
                      'winking':16,'sad':19,'crying':19,'angry':20,'surprised':21,'shocked':21,
                      'thinking':14,'embarrassed':17,'silly':17,'confused':17,'sleepy':18,
                      'neutral':0,'idle':0,'relaxed':0,'unknown-test':0}
            for name,pose in emotions.items():
                patterns=[rf'probe emotion={name}']
                if pose!=0: patterns.append(rf'Character frame pose={pose} submitted=1')
                check('emotion-'+name,'emotion '+name,5.6,patterns)
            actions={'wave':(1,6),'chin':(5,5),'rub':(6,5.4),'bubble':(3,7),'heart':(7,4.8),'fish':(4,6.5),'peek':(8,6),'shaker':(9,7),'drum':(10,7),'keys':(11,7),'guitar':(2,6)}
            for name,(pose,duration) in actions.items():
                check('action-'+name,'action '+name,duration+1.5,
                      [rf'action scheduled accepted=1 name={name}',rf'Character frame pose={pose} submitted=1',rf'Character frame pose={18 if name=="rub" else 0} submitted=1'])
                check('interrupt-start-'+name,'action '+name,1,[rf'Character frame pose={pose} submitted=1'])
                check('interrupt-'+name,'cancel',1,[r'Character frame pose=0 submitted=1'])
            check('static-fallback','fallback',2,[r'Character static fallback',r'probe fallback=1'])
            check('recover','recover',2,[r'Character frame pose=0 submitted=1'])
            if args.music:
                check('music-start','music-play',12,[r'probe music accepted=1',r'Companion foreground running=1'])
                check('music-listen','state listen',4,[r'Character frame pose=13 submitted=1',r'Companion foreground running=1'])
                check('music-speak','state speak',4,[r'Character frame pose=15 submitted=1',r'Companion foreground running=1'])
                check('music-switch','music-play',12,[r'probe music accepted=1',r'Companion foreground running=1'])
                check('music-stop','music-stop',7,[r'audio_complete .*underruns=0 underrun_total_ms=0',r'Character frame pose=0 submitted=1'])
            record({'suite':'PASS','tests':len(results),'limitations':['software injection, not ASR','no physical screen capture','long-duration test skipped']})
            print(f'PASS: {len(results)} tests; {args.output}',flush=True)
        except BaseException as exc:
            record({'suite':'FAIL','error':str(exc),'tests_completed':len(results)})
            raise
        finally:
            send('cancel'); send('recover')
            if args.music: send('music-stop')


if __name__=='__main__':
    main()
