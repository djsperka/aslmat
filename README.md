# aslmat

MATLAB mex file for monitoring eye position using the ASL eye tracker. 

## compilation

`mex asl.cpp`

## usage

```matlab
asl('connect', '128.120.140.228:51000');
asl('read');
asl('disconnect');
```

### connect

`asl('connect', 'addr:port')`

Opens a TCP connection to the ASL eye tracker at `addr:port`. `addr` can be an IP address or a hostname.

Once connected, a background thread reads eye data from the tracker at 120Hz. On each pass it reads every complete sample already waiting and keeps only the newest, so the saved sample stays current even if the tracker sends data faster than 120Hz.

Raises an error if:
- already connected (call `asl('disconnect')` first)
- the address is not in the form `'addr:port'`
- the connection cannot be made

### read

`asl('read')`

Displays the most recent eye data sample saved by the background thread. It does not wait for new data. Each sample contains:

| Field | Description |
|---|---|
| `status` | 0 = normal, 1 = calibration |
| `calibration_point_number` | Calibration point number |
| `frame_number` | Frame number from the tracker |
| `pupil_diameter` | Pupil diameter |
| `gaze_x`, `gaze_y` | Gaze position |

Raises an error if:
- not connected
- no sample has been received since connecting

If the background thread stops because of an error, such as the tracker closing the connection, the module disconnects itself and prints a message giving the reason. `asl('status')` also shows the reason. You can call `asl('connect', ...)` again right away, without calling `asl('disconnect')` first.

### disconnect

`asl('disconnect')`

Stops the background thread and closes the connection. Raises an error if not connected.

