import array
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import wave

RATE=48000
FRAME=960
DELAY=960


def require(condition,message):
    if not condition:
        raise AssertionError(message)


def pcm(values):
    return array.array("h",(max(-32768,min(32767,round(value))) for value in values))


def rms(values):
    return math.sqrt(sum(value*value for value in values)/max(1,len(values)))


def decibels(ratio):
    return 20*math.log10(max(ratio,1e-12))


def render(executable,source,strength,directory,mode=""):
    input_path=directory/"input.raw"
    output_path=directory/"output.raw"
    data=array.array("h",source)
    if sys.byteorder!="little":
        data.byteswap()
    input_path.write_bytes(data.tobytes())
    command=[executable,str(strength),str(input_path),str(output_path)]
    if mode:
        command.append(mode)
    subprocess.run(command,check=True)
    result=array.array("h")
    result.frombytes(output_path.read_bytes())
    if sys.byteorder!="little":
        result.byteswap()
    require(len(result)==len(source),"Processor changed the stream length")
    return result


def speech_score(reference,output):
    energy=sum(value*value for value in reference)
    gain=sum(a*b for a,b in zip(reference,output))/max(energy,1)
    error=sum((b-gain*a)**2 for a,b in zip(reference,output))
    return 10*math.log10(max(gain*gain*energy,1)/max(error,1)),gain


def read_speech(path):
    with wave.open(str(path),"rb") as source:
        require(source.getnchannels()==1 and source.getsampwidth()==2,"Speech fixture must be 16-bit mono WAV")
        rate=source.getframerate()
        samples=array.array("h",source.readframes(source.getnframes()))
    if sys.byteorder!="little":
        samples.byteswap()
    output=[]
    for index in range(len(samples)*RATE//rate):
        position=index*rate/RATE
        left=int(position)
        fraction=position-left
        output.append(samples[left]*(1-fraction)+samples[min(left+1,len(samples)-1)]*fraction)
    scale=14000/max(max(abs(value) for value in output),1)
    return [value*scale for value in output]


def test_noise(executable,directory):
    rng=random.Random(4242)
    source=pcm(rng.gauss(0,900) for _ in range(RATE*4))
    for strength in (-1,0):
        require(render(executable,source,strength,directory)==source,"Disabled/zero suppression is not bit-exact bypass")
    dry=source[RATE:RATE*3]
    previous=rms(dry)
    for strength in (1,25,50,75,100):
        output=render(executable,source,strength,directory)
        wet=output[RATE+DELAY:RATE*3+DELAY]
        level=rms(wet)
        require(level<previous,"Suppression strength does not progressively reduce noise")
        previous=level
        if strength==1:
            require(rms([b-a for a,b in zip(dry,wet)])/rms(dry)<0.02,"1% applies a hidden full-strength filter or misaligns the dry signal")
        print(f"Noise strength {strength:3}: {decibels(rms(dry)/max(level,1e-9)):.2f} dB reduction")
    require(decibels(rms(dry)/max(previous,1e-9))>6,"100% suppression leaves stationary noise essentially unchanged")
    repeated=render(executable,source,100,directory,"refresh")
    require(repeated==output,"Repeated settings updates reset denoiser state")
    toggled=render(executable,source,100,directory,"toggle")
    for start in range(0,len(source),FRAME):
        if start//FRAME%50>=25:
            require(toggled[start:start+FRAME]==source[start:start+FRAME],"Disabled filter leaks delayed audio")
    silence=pcm([0]*(RATE*2))
    require(not any(render(executable,silence,100,directory)),"Silence creates nonzero output")
    for level in (100,4000):
        scaled=pcm(value*level/900 for value in source)
        filtered=render(executable,scaled,100,directory)
        reduction=decibels(rms(scaled[RATE:RATE*3])/max(rms(filtered[RATE+DELAY:RATE*3+DELAY]),1e-9))
        require(reduction>5,f"Suppression ineffective at PCM noise RMS {level}: {reduction:.2f} dB")
        print(f"Noise RMS {level}: {reduction:.2f} dB reduction")


def test_speech(executable,path,directory):
    speech=read_speech(path)
    clean=[0.0]*(RATE*2)+speech+[0.0]*(RATE*2)
    clean.extend([0.0]*((-len(clean))%FRAME))
    rng=random.Random(4242)
    noisy=pcm(value+rng.gauss(0,900) for value in clean)
    start=RATE*2
    stop=start+len(speech)
    reference=clean[start:stop]
    before,_=speech_score(reference,noisy[start:stop])
    result=render(executable,noisy,100,directory)
    after,gain=speech_score(reference,result[start+DELAY:stop+DELAY])
    require(after-before>3,f"Speech-plus-noise SI-SDR improvement only {after-before:.2f} dB")
    require(0.7<gain<1.15,f"Suppression destroys speech level: {gain:.3f}")
    clean_output=render(executable,pcm(clean),100,directory)
    clean_score,clean_gain=speech_score(reference,clean_output[start+DELAY:stop+DELAY])
    require(clean_score>12 and 0.7<clean_gain<1.15,"Clean speech is excessively distorted or attenuated")
    print(f"Speech plus noise SI-SDR: {before:.2f} -> {after:.2f} dB, speech gain {gain:.3f}")
    print(f"Clean speech SI-SDR: {clean_score:.2f} dB, speech gain {clean_gain:.3f}")


def main():
    executable=str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory() as temporary:
        directory=Path(temporary)
        test_noise(executable,directory)
        if len(sys.argv)>2 and sys.argv[2]:
            test_speech(executable,Path(sys.argv[2]),directory)
        else:
            print("Speech fixture absent: speech preservation test not run")


if __name__=="__main__":
    main()
