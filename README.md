# Canoroid

ถ่ายภาพสไตล์กล้อง instant บน Canon EOS 600D ด้วยฐาน Magic Lantern รุ่นทดลองนี้นำ JPEG จากการ์ดมาครอปตรงกลางเป็น 1:1 เลือกกรอบได้เก้าแบบ และบันทึกภาพสีขนาด 1080 × 1080 เพิ่มอีกไฟล์

## Canoroid v0.1.0-alpha (รุ่นที่ 42)

เลือก JPEG หรือ RAW+JPEG ในเมนูคุณภาพของ Canon ก่อนถ่ายด้วย Canoroid หากเลือก RAW อย่างเดียว หน้าเลือกกรอบจะแจ้งให้กด MENU เพื่อเปลี่ยนคุณภาพ


หน้าแรกแบ่งเป็น Film tones สำหรับเลือกโทนถ่ายภาพ และ Photos & Game สำหรับ View Photos กับ Game ใน DinoFrame Run กด SET เพื่อเริ่มหรือกระโดด กดลงค้างเพื่อหลบ กด Q เพื่อพัก และ PLAY เพื่อกลับหน้าแรก


สำหรับ **Canon EOS 600D เฟิร์มแวร์ Canon 1.0.2** เลือกกรอบแล้วกด SET เพื่อเข้า Live View ถ่าย JPEG หรือ RAW+JPEG ทีละภาพ แล้วเปรียบเทียบต้นฉบับกับภาพใส่กรอบได้ในหน้าผลลัพธ์ รุ่นที่ 42 ใช้รุ่น 26 เป็นฐาน เปลี่ยนชื่อโทนทั้งสี่ แสดงหน้าปก 1 วินาที และเปิด View Photos บนการ์ดใหม่ได้ ภาพใหม่อยู่ใน DCIM/CANOROID รุ่นนี้ผ่านการบิลด์ ARM และทดสอบบนคอมพิวเตอร์แล้ว ยังรอตรวจจอบน 600D จริง

- ติดตั้งครั้งแรก มีตัวติดตั้ง ML-SETUP.FIR — [ดาวน์โหลด FIRST-INSTALL](https://raw.githubusercontent.com/pongallery/Canoroid/v0.1.0-alpha.42/downloads/Canoroid-v0.1.0-alpha.42-FIRST-INSTALL.zip)
- อัปเดตกล้องและการ์ดที่บูต Magic Lantern ได้อยู่แล้ว — [ดาวน์โหลด UPDATE](https://raw.githubusercontent.com/pongallery/Canoroid/v0.1.0-alpha.42/downloads/Canoroid-v0.1.0-alpha.42-UPDATE.zip)
- [ตรวจสอบ SHA-256](downloads/SHA256SUMS.txt)

แตก ZIP แล้วคัดลอกไฟล์ด้านในไปที่รากการ์ด SD ดูวิธีเตรียมภาพและใช้งานปุ่มกล้องใน [คู่มือ Canoroid](firmware/CANOROID.md)

## การพัฒนา

ซอร์สและเครื่องมือบิลด์อยู่ใน [firmware](firmware/) ตรวจขั้นตอน Live View การบันทึก และการเปิดดูผลบน EOS 600D ก่อนเพิ่ม LUT

Canoroid ใช้ Semantic Versioning โดยใช้แท็ก vMAJOR.MINOR.PATCH และส่วนต่อท้าย alpha.N สำหรับรุ่นทดลอง

## เครดิตและใบอนุญาต

พอร์ตกล้องและระบบพื้นฐานมาจาก Magic Lantern ตัวแปลง JPEG ใช้ซอฟต์แวร์ของ Independent JPEG Group รายละเอียดอยู่ใน [เครดิต](firmware/CANOROID.md#เครดิต) และ [ใบอนุญาตโครงการ](LICENSE)
