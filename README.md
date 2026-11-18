# Canoroid

ถ่ายภาพสไตล์กล้อง instant บน Canon EOS 600D ด้วยฐาน Magic Lantern รุ่นทดลองนี้นำ JPEG จากการ์ดมาครอปตรงกลางเป็น 1:1 เลือกกรอบได้สามแบบ และบันทึกภาพสีขนาด 1080 × 1080 เพิ่มอีกไฟล์

## ดาวน์โหลด v0.1.0-alpha.2

สำหรับ **Canon EOS 600D เฟิร์มแวร์ Canon 1.0.2** รุ่นนี้คือ overlay test build 2 ซึ่งปรับจังหวะเปิดและวาดหน้าหลักหลัง Canon พร้อม บิลด์ ARM และการประมวลผล JPEG บนคอมพิวเตอร์ผ่านแล้ว การเปิดหน้าหลักและการดูผลบนกล้องจริงยังรอการยืนยัน

- ติดตั้งครั้งแรก มีตัวติดตั้ง ML-SETUP.FIR — [ดาวน์โหลด FIRST-INSTALL](https://raw.githubusercontent.com/pongallery/Canoroid/v0.1.0-alpha.2/downloads/Canoroid-0.1.0-alpha.2-EOS600D-FW1.0.2-FIRST-INSTALL.zip)
- อัปเดตกล้องและการ์ดที่บูต Magic Lantern ได้อยู่แล้ว — [ดาวน์โหลด UPDATE](https://raw.githubusercontent.com/pongallery/Canoroid/v0.1.0-alpha.2/downloads/Canoroid-0.1.0-alpha.2-EOS600D-FW1.0.2-UPDATE.zip)
- [ตรวจสอบ SHA-256](downloads/SHA256SUMS.txt)

แตก ZIP แล้วคัดลอกไฟล์ด้านในไปที่รากการ์ด SD ดูวิธีเตรียมภาพและใช้งานปุ่มกล้องใน [คู่มือ Canoroid](firmware/CANOROID.md)

## การพัฒนา

ซอร์สและเครื่องมือบิลด์อยู่ใน [firmware](firmware/) เริ่มจากทดสอบการบันทึกและเปิดภาพใส่กรอบบนกล้อง ก่อนเพิ่ม Live View การทำงานอัตโนมัติหลังถ่าย และ LUT

Canoroid ใช้ Semantic Versioning โดยใช้แท็ก vMAJOR.MINOR.PATCH และส่วนต่อท้าย alpha.N สำหรับรุ่นทดลอง เลข FW ในชื่อ ZIP ระบุเฟิร์มแวร์ Canon ที่รองรับ

## เครดิตและใบอนุญาต

พอร์ตกล้องและระบบพื้นฐานมาจาก Magic Lantern ตัวแปลง JPEG ใช้ซอฟต์แวร์ของ Independent JPEG Group รายละเอียดอยู่ใน [เครดิต](firmware/CANOROID.md#เครดิต) และ [ใบอนุญาตโครงการ](LICENSE)
