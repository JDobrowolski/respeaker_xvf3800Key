# tts_synth.ps1 - probki syntetyczne glosami Windows (SAPI) do startowego treningu.
# Pozytywy: slowo kluczowe w roznym tempie i wysokosci; negatywy: podobne slowa i zdania
# TYMI SAMYMI glosami (siec nie moze nauczyc sie "glos syntezatora = slowo").
# Probki z syntezatora tylko na start - dla dobrej skutecznosci nagraj prawdziwe glosy
# (kwstool record <ip> dataset\keyword).
#
#   powershell -ExecutionPolicy Bypass -File tts_synth.ps1 [-Word punapu] [-Out dataset]
param(
    [string]$Word = "punapu",
    [string]$Out = "dataset"
)
Add-Type -AssemblyName System.Speech
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voices = $s.GetInstalledVoices() | Where-Object { $_.Enabled } | ForEach-Object { $_.VoiceInfo.Name }

# zapis fonetyczny dla angielskich glosow (zblizony do polskiego "punapu")
$spell = @($Word, "poonahpoo", "poo nah poo", "poonapoo", "punahpu")
$rates = @("x-slow", "slow", "medium", "fast")
$pitch = @("x-low", "low", "medium", "high", "x-high")

$neg = @("puma", "papu", "napu", "puna", "pana", "Panama", "banana", "punkt", "pupa", "kapu", "tuna",
         "panorama", "bonanza", "lunapark", "Kahuna", "poona", "noona", "pun", "nap", "up", "Papua",
         "hello", "computer", "okay google", "alexa", "what time is it", "turn on the light",
         "good morning", "thank you", "open the door", "play some music", "how are you",
         "one two three", "yes", "no", "stop", "weather today", "pineapple", "Napoleon", "cappuccino",
         "Honolulu", "kangaroo", "peekaboo", "voodoo", "Timbuktu", "bamboo", "tofu", "menu")

function Say($voice, $text, $rate, $p, $path) {
    $ssml = "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>" +
            "<voice name='$voice'><prosody rate='$rate' pitch='$p'>$text</prosody></voice></speak>"
    $s.SetOutputToWaveFile($path, $fmt)
    $s.SpeakSsml($ssml)
    $s.SetOutputToNull()
}

$kd = Join-Path $Out "keyword"; $nd = Join-Path $Out "negative\tts"
New-Item -ItemType Directory -Force $kd, $nd | Out-Null
$k = 0
foreach ($v in $voices) { foreach ($t in $spell) { foreach ($r in $rates) { foreach ($p in $pitch) {
    if ((($k++) % 2) -eq 0) { Say $v $t $r $p (Join-Path $kd ("tts_{0:d4}.wav" -f $k)) }
} } } }
$n = 0
foreach ($v in $voices) { foreach ($t in $neg) { foreach ($r in @("slow", "medium", "fast")) {
    $p = $pitch[$n % $pitch.Count]
    Say $v $t $r $p (Join-Path $nd ("tts_{0:d4}.wav" -f ($n++)))
} } }
Write-Output ("glosy: {0}; slowo: {1} plikow w {2}; negatywy: {3} plikow w {4}" -f ($voices -join ", "), (Get-ChildItem $kd -Filter tts_*.wav).Count, $kd, $n, $nd)
