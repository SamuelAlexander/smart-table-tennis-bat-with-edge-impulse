#!/usr/bin/env python3
"""
Table Tennis Data Collector - USB Serial Version
Collects continuous IMU data from Arduino via USB serial connection at 50Hz.

Requirements:
- pyserial (pip install pyserial)
- Arduino Nano 33 BLE Sense Rev2 connected via USB

Author: TT Data Collection System
"""

import serial
import serial.tools.list_ports
import time
import sys
import os
import threading
from datetime import datetime

class TTDataCollector:
    def __init__(self):
        self.serial_conn = None
        self.recording = False
        self.samples_collected = 0
        self.csv_file = None
        self.start_time = None
        self.stop_recording = False
        
    def find_arduino(self):
        """Scan for Arduino on serial ports"""
        print("🔍 Scanning for Arduino...")
        
        # Get list of available ports
        ports = serial.tools.list_ports.comports()
        arduino_ports = []
        
        for port in ports:
            # Look for Arduino-like devices
            if any(keyword in port.description.lower() for keyword in ['arduino', 'nano', 'usb serial']):
                arduino_ports.append(port.device)
            elif 'cu.usbmodem' in port.device or 'ttyACM' in port.device:
                arduino_ports.append(port.device)
        
        if not arduino_ports:
            print("❌ No Arduino found. Please check connection.")
            return None
        
        # Try to connect to each potential Arduino
        for port in arduino_ports:
            try:
                print(f"🔌 Trying {port}...")
                ser = serial.Serial(port, 115200, timeout=2)
                time.sleep(3)  # Wait for Arduino to initialize
                
                # Clear any existing data in buffer
                ser.flushInput()
                
                # Send a test command to see if Arduino responds
                ser.write(b"TEST\n")
                time.sleep(0.5)
                
                # Check for any response (Arduino might send READY again or respond to TEST)
                response_received = False
                for attempt in range(10):  # Try for 5 seconds
                    if ser.in_waiting > 0:
                        try:
                            response = ser.readline().decode().strip()
                            print(f"🔍 Arduino response: '{response}'")
                            if response in ["READY", "STARTED", "STOPPED"] or "TT" in response:
                                print(f"✅ Arduino connected on {port}")
                                return ser
                            response_received = True
                        except:
                            pass
                    time.sleep(0.5)
                
                if not response_received:
                    print(f"❌ No response from {port}")
                
                ser.close()
                
            except Exception as e:
                print(f"❌ Failed to connect to {port}: {e}")
                continue
        
        print("❌ No Arduino found with correct firmware.")
        return None
    
    def get_filename(self):
        """Get filename from user input"""
        while True:
            filename = input("\n📁 Enter filename (e.g., FHdrive.csv): ").strip()
            
            if not filename:
                print("❌ Please enter a filename.")
                continue
            
            if not filename.endswith('.csv'):
                filename += '.csv'
            
            # Check if file exists
            if os.path.exists(filename):
                overwrite = input(f"⚠️  File {filename} exists. Overwrite? (y/n): ").strip().lower()
                if overwrite != 'y':
                    continue
            
            return filename
    
    def start_recording(self, filename):
        """Start recording session"""
        try:
            # Open CSV file
            self.csv_file = open(filename, 'w')
            self.csv_file.write("Timestamp,AccelX,AccelY,AccelZ,GyroX,GyroY,GyroZ\n")
            self.csv_file.flush()
            
            # Clear any pending data in serial buffer
            self.serial_conn.flushInput()
            time.sleep(0.1)
            
            # Send START command to Arduino
            self.serial_conn.write(b"START\n")
            
            # Wait for Arduino acknowledgment (be more tolerant)
            max_attempts = 10
            for attempt in range(max_attempts):
                try:
                    response = self.serial_conn.readline().decode().strip()
                    print(f"🔍 Arduino response: '{response}'")
                    
                    if response == "STARTED":
                        print("✅ Arduino confirmed START")
                        break
                    elif response == "READY":
                        print("⏳ Got READY, sending START again...")
                        self.serial_conn.write(b"START\n")
                    elif response == "":
                        print("⏳ No response, retrying...")
                        self.serial_conn.write(b"START\n")
                        
                except Exception as e:
                    print(f"⚠️ Communication error: {e}")
                
                time.sleep(0.5)
                
                if attempt == max_attempts - 1:
                    print("❌ Failed to get START confirmation from Arduino")
                    return False
            
            self.recording = True
            self.samples_collected = 0
            self.start_time = time.time()
            self.stop_recording = False
            
            print(f"🔴 Recording started - {filename}")
            print("📊 Press 'q' + ENTER to stop recording")
            print("⏱️  Max recording time: 2 minutes")
            print("-" * 50)
            
            return True
            
        except Exception as e:
            print(f"❌ Failed to start recording: {e}")
            return False
    
    def stop_recording_session(self):
        """Stop recording session"""
        if not self.recording:
            return
        
        try:
            # Send STOP command to Arduino
            self.serial_conn.write(b"STOP\n")
            
            # Wait for Arduino acknowledgment
            response = self.serial_conn.readline().decode().strip()
            if response == "STOPPED":
                print("\n✅ Arduino stopped successfully")
            
            self.recording = False
            
            # Close CSV file
            if self.csv_file:
                self.csv_file.close()
                self.csv_file = None
            
            elapsed_time = time.time() - self.start_time
            print(f"🎯 Recording completed!")
            print(f"📊 Samples collected: {self.samples_collected}")
            print(f"⏱️  Duration: {elapsed_time:.1f} seconds")
            print(f"📈 Average sample rate: {self.samples_collected/elapsed_time:.1f} Hz")
            
        except Exception as e:
            print(f"❌ Error stopping recording: {e}")
    
    def data_collection_thread(self):
        """Thread for collecting data from Arduino"""
        max_duration = 120  # 2 minutes in seconds (yields ~6,000 samples at 50Hz)
        
        while self.recording and not self.stop_recording:
            try:
                # Check for timeout
                elapsed = time.time() - self.start_time
                if elapsed >= max_duration:
                    print(f"\n⏰ Maximum recording time ({max_duration/60:.1f} minutes) reached!")
                    self.stop_recording = True
                    break
                
                # Read data from Arduino
                if self.serial_conn.in_waiting > 0:
                    line = self.serial_conn.readline().decode().strip()
                    
                    # Skip empty lines and command responses
                    if not line or line in ['READY', 'STARTED', 'STOPPED']:
                        continue
                    
                    # Write to CSV file
                    self.csv_file.write(line + '\n')
                    self.csv_file.flush()  # Ensure data is written immediately
                    
                    self.samples_collected += 1
                    
                    # Update display every 100 samples
                    if self.samples_collected % 100 == 0:
                        elapsed = time.time() - self.start_time
                        rate = self.samples_collected / elapsed if elapsed > 0 else 0
                        print(f"📊 Samples: {self.samples_collected:,} | Time: {elapsed:.1f}s | Rate: {rate:.1f} Hz", end='\r')
                
                time.sleep(0.001)  # Small delay to prevent busy waiting
                
            except Exception as e:
                print(f"\n❌ Error during data collection: {e}")
                break
        
        # Stop recording
        self.stop_recording_session()
    
    def user_input_thread(self):
        """Thread for handling user input"""
        while self.recording:
            try:
                user_input = input().strip().lower()
                if user_input == 'q':
                    print("\n🛑 Stopping recording...")
                    self.stop_recording = True
                    break
            except EOFError:
                break
            except KeyboardInterrupt:
                print("\n🛑 Stopping recording...")
                self.stop_recording = True
                break
    
    def run_session(self):
        """Run a single recording session"""
        filename = self.get_filename()
        
        if not self.start_recording(filename):
            return False
        
        # Start data collection thread
        data_thread = threading.Thread(target=self.data_collection_thread)
        data_thread.daemon = True
        data_thread.start()
        
        # Start user input thread
        input_thread = threading.Thread(target=self.user_input_thread)
        input_thread.daemon = True
        input_thread.start()
        
        # Wait for threads to complete
        data_thread.join()
        
        return True
    
    def run(self):
        """Main program loop"""
        print("🏓 Table Tennis Data Collector v2.0")
        print("=" * 50)
        
        # Find and connect to Arduino
        self.serial_conn = self.find_arduino()
        if not self.serial_conn:
            return
        
        try:
            while True:
                print("\n" + "=" * 50)
                print("📋 Main Menu:")
                print("1. Start new recording session")
                print("2. Quit")
                
                choice = input("\nEnter choice (1-2): ").strip()
                
                if choice == '1':
                    self.run_session()
                    
                elif choice == '2':
                    print("👋 Goodbye!")
                    break
                    
                else:
                    print("❌ Invalid choice. Please enter 1 or 2.")
        
        except KeyboardInterrupt:
            print("\n\n🛑 Program interrupted by user")
        
        finally:
            if self.recording:
                self.stop_recording_session()
            
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.close()
                print("🔌 Serial connection closed")

def main():
    """Main entry point"""
    collector = TTDataCollector()
    collector.run()

if __name__ == "__main__":
    main()
