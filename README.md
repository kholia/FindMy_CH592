# CH592 Find My tracker firmware

This firmware supports Apple Find My, Google's Find My Device / Find Hub
Network, or both networks from one CH592 binary. Dual mode alternates complete
Apple and Google advertisements every 30 seconds because each frame consumes
nearly all of the 31-byte legacy BLE advertising payload.

### Usage

Get https://github.com/WeActStudio/WeActStudio.WCH-BLE-Core 'CH592F' board(s).

![CH592F board](./CH592F-Board.png)

Set up the repository:

```
git clone https://github.com/kholia/FindMy_CH592.git && cd FindMy_CH592

python3 -m venv venv

source venv/bin/activate

pip install -r requirements.txt
```

Generate one or more key(s):
   
```
./generate_keys.py
```

Prepare an Apple Find My firmware:

```
./prep_fw.py --keyfile first.keys second.keys third.keys --adv-interval=2 --firmware main.bin --output flashme.bin
```

For Google Find Hub, register the tracker using
`GoogleFindMyTools-main/main.py`, then copy the 40-character
`Advertisement Key` printed by that flow:

```
./prep_fw.py --google-eid 00112233445566778899aabbccddeeff00112233 \
  --adv-interval 3 --firmware main.bin --output flashme.bin
```

To advertise on both networks, pass both credential types:

```
./prep_fw.py --keyfile device.keys \
  --google-eid 00112233445566778899aabbccddeeff00112233 \
  --adv-interval 3 --firmware main.bin --output flashme.bin
```

Dual mode alternates networks every 30 seconds. Multiple Apple keys still
rotate hourly. Network switching does not change the Bluetooth address; only
an Apple key rotation changes it. Google-only mode uses the CH592 factory
address, matching the reference Google firmwares, and uses `--google-eid` only
for the FMDN service-data EID.

After provisioning, `prep_fw.py` prints the expected initial Apple address and
Google advertisement EID. Compare those values with an on-air capture to catch
an incorrect credential or accidentally flashed image immediately.

The Google support matches the experimental simple beacon in
`GoogleFindMyTools-main`: it advertises one fixed 20-byte EID and does not
implement Fast Pair or on-device EID derivation. Its current workflow requires
periodically re-announcing the advertisement key to Google, and locations are
queried through GoogleFindMyTools rather than the official app.

Flash the firmware:

```
./flash.sh
```

### Further preparation [OPTIONAL]

```
sqlite3 reports.db 'CREATE TABLE reports (id_short TEXT, timestamp INTEGER, datePublished INTEGER, payload TEXT, id TEXT, statusCode INTEGER, PRIMARY KEY(id_short,timestamp))'
```

```
docker network create mh-network

docker run -d --restart always --name anisette -p 6969:6969 --volume anisette-v3_data:/home/Alcoholic/.config/anisette-v3 --network mh-network dadoum/anisette-v3-server
```

Note: You will be asked for your Apple-ID, password and your 2FA. If you see
serving at port 6176 over HTTP you have all set up correctly. 

```
$ ./request_reports.py
pyprovision is not installed, querying http://localhost:6969 for an anisette server
200: 8 reports received.
...
```

### References

- Android app => https://github.com/parawanderer/OpenTagViewer - Use this!

  ```
  qrencode -r XYZ_devices.json -o sensitive.png

  open sensitive.png
  ```

  Scan this with the `OpenTagViewer` Android app

- https://github.com/biemster/qible

- https://github.com/dchristl/macless-haystack

- https://github.com/biemster/FindMy

- https://github.com/malmeloo/FindMy.py

- https://www.wch.cn/downloads/CH592EVT_ZIP.html

### PS

- While you can create HW tags at a (much) lower cost, this work presents a
  good balance of convenience, cost, and performance.

- Use `nRF Connect for Mobile` Android app for seeing these beacons in action.
