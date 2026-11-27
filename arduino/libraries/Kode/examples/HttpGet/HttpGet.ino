/* SPDX-License-Identifier: GPL-3.0-or-later */
// Joins the Wi-Fi network the board remembers and fetches a page every ten seconds.
// A call that fails says why in Kode.error(), which is read before the next call to Kode.

const char *PAGE_URL = "https://example.com";  // The page to fetch.
const uint32_t PAGE_EVERY_MS = 10000;          // How often it is fetched.

void setup() {
  Serial.begin(115200);
}

void loop() {
  if (!Kode.wifi.isConnected() && !Kode.wifi.connect()) {
    Serial.print("No network: ");
    Serial.println(Kode.error());
  } else {
    const char *page = Kode.http.get(PAGE_URL);
    String why = Kode.error();
    if (why.length() > 0) {
      Serial.print("No page: ");
      Serial.println(why);
    } else {
      Serial.print("HTTP ");
      Serial.print(Kode.http.status());
      Serial.print(", ");
      Serial.print(strlen(page));
      Serial.println(" bytes");
    }
  }
  delay(PAGE_EVERY_MS);
}
