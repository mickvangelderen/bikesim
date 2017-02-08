# bikesim

This project runs a bicycle simulator running with a monitor and a modified
stationary bicycle interface.

The software consists of 3 parts:
 - A Unity3d game/project that runs on a computer that runs the game engine and
   simulates the bicycle dynamics.
 - An arduino project that reads various sensors and actuates the haptic motor.
 - A python layer that interfaces between the game and embedded device.

## Generation of protobuf sources

Because phobos' protobuf messages use proto2 and the original protobuf
codegenerator does not support C#, we have to use a tool that can be downloaded
[here](
https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/protobuf-net/protobuf-net%20r668.zip).

Unfortunately, this protogen tool must be run on Windows. Assuming you download
and extract the contents to `ignore\protobuf`, issue the following command to
generate classes for your protobuf messages.

```cmd
"ignore\protobuf\ProtoGen\protogen.exe" "-i:bikesim\Assets\proto\simulation.proto" "-o:bikesim\Assets\Scripts\simulation.cs" "-ns:pb"
```

Generation of new protobuf sources is only required when the protobuf message
definition is updated.

## Acknowledgements
This repository is forked from a project started by a Bachelor/Master student
group at TU Delft.

A number of people have contributed this project including:
Marco Grottoli, Jodi Kooijman, Thom van Beek, Guillermo Currás Lorenzo, and
Tiago Pinto
