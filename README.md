# CoCanMirror 路 灏忕數鎷煎姛鐜囩洃鎺э紙AI Passport锛?
鎶?FoloToy AI Passport 鍙樻垚灏忕數鎷硷紙CoCan锛夊厖鐢靛櫒鐨勯殢韬姛鐜囦华琛ㄣ€?
![鐣岄潰](docs/cocan/cover.png)

## 鑳藉仛浠€涔?
- **鎬昏**锛氭€昏緭鍑哄姛鐜囥€?60W 鍗犵敤姣斾緥銆? 涓帴鍙ｇ殑瀹炴椂鐢靛帇 / 鐢垫祦 / 鍔熺巼
- **鎺ュ彛璇︽儏**锛氬姛鐜囥€佺數鍘嬬數娴併€佸揩鍏呭崗璁€佹渶杩?60 绉掓洸绾?- **鎬昏緭鍑鸿鎯?*锛氭€诲姛鐜囦笌鍗犳瘮銆佸悇鎺ュ彛鐘舵€侊紙OFF / 寰呮満 / 杈撳嚭涓級銆?0 绉掓洸绾?- **鎺ュ彛寮€鍏?*锛氳鎯呴〉闀挎寜 OK 鍒囨崲鍗曞彛锛涙€昏緭鍑洪〉闀挎寜 OK 鎸夊綋鍓嶇姸鎬佸紑鍚?/ 鍏抽棴鍏ㄩ儴鎺ュ彛
- **璁惧绔厤缃?*锛氶娆″紑鏈鸿繛 `AIPassport-XXXX` 鐑偣锛屾墜鏈鸿嚜鍔ㄥ脊鍑洪厤缃〉锛屽垪琛ㄩ€夋嫨 Wi-Fi銆佸～鍐欏皬鐢垫嫾 MCP 鍦板潃
- **璁剧疆**锛氳疆璇㈤€熷害 0.5 / 1 / 3 绉掋€佹伅灞忚疆璇€佹伅灞忔椂闂淬€佷寒搴︺€佷腑鑻辨枃

## 涓嬭浇涓庡畨瑁?
鍥轰欢鍦?[Releases](https://github.com/Kiiiiro/CoCanMirror-passport/releases/tag/v2.6.3) 涓嬭浇锛?
```bash
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 FoloToy-AI-Passport-2.6.3-public.bin
```

SHA256锛歚0f86477c912598666a00ec757c1adfb711768083a901fba1ebe4e7a6c4498e70`

## 浠庢簮鐮佹瀯寤?
闇€瑕?ESP-IDF 5.5.3锛?
```bash
idf.py set-target esp32c3
idf.py build
```

## 璇存槑

- 浠呮敮鎸佷簯绔?MCP锛岄渶瑕佽仈缃戝拰鏈夋晥 MCP 鍦板潃
- 绾跨紗鑺墖璇︽儏锛圲SB 閫熺巼 / 绾挎潗瑙勬牸 / VID锛夋殏涓嶆樉绀猴細鏈嶅姟绔崟娆¤繑鍥炵害 12KB锛岃秴鍑鸿澶囩紦鍐?- 浜戠杩炴帴涓哄姞瀵嗕絾涓嶆牎楠岃瘉涔︼紝寤鸿杩炴帴鍙俊 Wi-Fi
- 鏇村璇存槑瑙?[docs/cocan/README.md](docs/cocan/README.md)

## 鑷磋阿

鍩轰簬 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 寮€鍙戯紝閮ㄥ垎璁捐鎬濊矾鍙傝€?CP02 鐢垫簮鐩戞帶椤圭洰銆?