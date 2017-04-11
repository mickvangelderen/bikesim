using System;
using System.Threading;
using System.IO;
using System.IO.Ports;
using UnityEngine;
using ProtoBuf;


public class SerialThread {
    private string _portname;
    private Int32 _baudrate;

    private const int _read_timeout = 1; // milliseconds
    private const int _write_timeout = 1; // milliseconds

    private SerialPort _port;

    private Thread _thread;
    private const string _threadname = "serial";
    private bool _should_terminate;

    // For phobos::flimnap, a packet consists of a pose_t, framed using COBS.
    // A pose_t is a packed struct of 29 bytes + 1 byte overhead from COBS + 1
    // byte delimiter = 31 bytes total
    private const int _buffer_size = 64;
    private byte[] _buffer_one;
    private byte[] _buffer_two;
    private int _buffer_offset;
    private byte[] _active_buffer;
    private byte[] _inactive_buffer;

    private int _packet_start;
    private int _packet_size;
    private string _gitsha1;
    private System.Text.Encoding _ascii;
    private readonly object _pose_lock;
    private pb.BicyclePoseMessage _pose;
    private int _packet_error;

    private System.IO.FileStream _file;

    public SerialThread(string portname, Int32 baudrate) {
        _portname = portname;
        _baudrate = baudrate;

        _port = null;
        _thread = null;
        _should_terminate = false;

        _buffer_one = new byte[_buffer_size];
        _buffer_two = new byte[_buffer_size];
        _buffer_offset = 0;

        _active_buffer = _buffer_one;
        _inactive_buffer = _buffer_two;

        _packet_start = 0;
        _packet_size = 0;
        _gitsha1 = null;
        _ascii = new System.Text.ASCIIEncoding();
        _pose_lock = new object();
        _pose = null;

        _packet_error = 0;
        int log_number = 0;
        _file = null;
        while (_file == null) {
            try {
                _file = new System.IO.FileStream(
                        String.Format("pose{0}.log", log_number),
                        System.IO.FileMode.CreateNew,
                        System.IO.FileAccess.Write,
                        System.IO.FileShare.Read);
                Debug.Log(String.Format("writing log to file: {0}", _file.Name));
            } catch (System.IO.IOException e) {
                ++log_number;
            }
        }
    }

    public string portname {
        get { return _portname; }
    }

    public Int32 baudrate {
        get { return _baudrate; }
    }

    public string gitsha1 {
        get { return _gitsha1; }
    }

    public bool IsOpen {
        get { return _port.IsOpen; }
    }

    private void OpenPort() {
        _port = new SerialPort(_portname, _baudrate);
        _port.ReadTimeout = _read_timeout;
        _port.WriteTimeout = _write_timeout;
        _port.Open();
    }

    private void ClosePort() {
        if (_port != null) {
            _port.Close();
            _port = null;
        }
    }

    public void Start() {
        OpenPort();
        _thread = new Thread(new ThreadStart(ThreadFunction));
        _thread.Name = _threadname;
        _thread.IsBackground = true;
        _thread.Start();
    }

    public void Stop() {
        _should_terminate = true;
        if (_thread == null) {
            return;
        }

        ClosePort();
        if ((_thread.ThreadState &
             (ThreadState.Stopped | ThreadState.Unstarted)) != 0) {
            _thread = null;
            return;
        }
        _thread.Abort();
        if (_thread.Join(100)) { // milliseconds
            _thread = null;
        }

        _file.Close();
    }

    private void SwapByteBuffer(byte[] current_buffer, int buffer_offset, int buffer_length, byte[] next_buffer) {
        Buffer.BlockCopy(current_buffer, buffer_offset, next_buffer, 0, buffer_length);
        _active_buffer = next_buffer;
        _inactive_buffer = current_buffer;
        _buffer_offset = buffer_length;
        _packet_start = 0;
        _packet_size = 0; // if an unstuffed packet was stored in the inactive buffer, it's now been overwritten
        _file.Write(_inactive_buffer, 0, buffer_offset); // save received data to file
    }

    private void ThreadFunction() {
        _should_terminate = false;

        // TODO: Use delegates instead of a sleep loop
        while (!_should_terminate) {
            if (!_port.IsOpen) {
                OpenPort();
            } else {
                int bytes_read = 64;
                try {
                    bytes_read = _port.BytesToRead;
                } catch (NullReferenceException) {
                    // This happens on Windows
                }
                if (bytes_read > 0) {
                    if (bytes_read > (_buffer_size - _buffer_offset)) {
                        bytes_read = _buffer_size - _buffer_offset;
                    }
                    try {
                        bytes_read = _port.Read(_active_buffer, _buffer_offset, bytes_read);
                    } catch (TimeoutException) {
                        // If on Windows, and aa NullRefernceException is thrown when defining bytes_read,
                        // Read() may timeout if the true number of bytes to is zero.
                        bytes_read = 0;
                    }
                    if (bytes_read > 0) {
                        int delimiter_index = Array.IndexOf(_active_buffer, (byte)0, _buffer_offset, bytes_read);
                        if (delimiter_index >= 0) {
                            while (delimiter_index >= 0) {
                                UnstuffPacket(_active_buffer, _packet_start, delimiter_index - _packet_start + 1);
                                PushReceivedPacket();

                                // Continue to process received bytes as long as another packet exists
                                bytes_read -= (delimiter_index - _buffer_offset + 1);
                                _buffer_offset = delimiter_index + 1;
                                delimiter_index = Array.IndexOf(_active_buffer, (byte)0, _buffer_offset, bytes_read);
                            }
                            SwapByteBuffer(_active_buffer, _buffer_offset, bytes_read, _inactive_buffer);
                        } else {
                            _buffer_offset += bytes_read;
                        }
                    }
                }
            }
            Thread.Sleep(0); // zero argument value results in a thread yield
        }
    }

    // Frame unstuff function here. This is copied from phobos/src/packet/frame.cc
    // TODO: Move framing (and serialization) code to separate files
    private int UnstuffPacket(byte[] buffer, int offset, int length) {
        _packet_start += length;
        int read_index = offset;
        int write_index = 0;

        while (read_index < offset + length) {
            byte code = buffer[read_index++];

            if ((read_index + code > length + offset) && (code != 1)) {
                // Distance code can exceed packet length if we start decoding
                // in the middle of a packet. We can return now and handle the
                // next packet correctly.
                _packet_size = 0;
                return 0;
            }

            for (byte i = 1; i < code; ++i) {
                _inactive_buffer[write_index++] = buffer[read_index++];
            }

            if ((code < 0xFF) && (read_index != length)) {
                _inactive_buffer[write_index++] = 0;
            }
        }
        System.Diagnostics.Debug.Assert((write_index - 1) == (length - 2),
                "number of unstuffed bytes does not match buffer length");
        _packet_size = write_index - 1;
        return _packet_size;
    }

    private void PushReceivedPacket() {
  		if (_packet_size != 0) {
  			using (var stream = new MemoryStream (_inactive_buffer)) {
  				lock (_pose_lock) {
            pb.SimulationMessage m = Serializer.Deserialize<pb.SimulationMessage>(stream);
  					_pose = m.pose;
  				}
  			}
  		}
    }

    public pb.BicyclePoseMessage PopBicyclePose() {
        pb.BicyclePoseMessage pose = null;
        lock(_pose_lock) {
            pose = _pose;
            _pose = null;
        }
        return pose;
    }
}
