#!/bin/bash
# Telegram <-> MQTT Bridge Script

# === CONFIGURATION ===
TELEGRAM_BOT_TOKEN="1234568:AAasdasdasdghfjytjjJtS5CLDTIbs"
declare -A USERS
USERS[123456789]="Aditya" #admin
USERS[987654321]="TEST"

MQTT_BROKER="localhost"
MQTT_PORT=1883
MQTT_USER="USER_NAME"
MQTT_PASS="P@SSW0RD"
MQTT_TOPIC="/topic/subtopic/"

MQTT_MSG_1="Miss you"
MQTT_MSG_2="Online"
MQTT_MSG_3="LOW BATT"
MQTT_MSG_4="DEEP SLEEP"

POLL_INTERVAL=5
OFFSET=0

# === FUNCTIONS ===

send_telegram_message() {
    local chat_id="$1"
    local message="$2"
    curl -s -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
        -d chat_id="$chat_id" \
        -d text="$message" > /dev/null
}

publish_mqtt_message() {
    local topic="$1"
    local message="$2"
    mosquitto_pub -h "$MQTT_BROKER" -p "$MQTT_PORT" -u "$MQTT_USER" -P "$MQTT_PASS" \
        -t "$topic" -m "$message"
}

mqtt_listener() {
    echo "[INFO] Starting MQTT -> Telegram bridge..."
    echo "[INFO] Listening to topic: $MQTT_TOPIC"
    mosquitto_sub -h "$MQTT_BROKER" -p "$MQTT_PORT" -u "$MQTT_USER" -P "$MQTT_PASS" \
        -t "$MQTT_TOPIC" | while IFS= read -r payload; do
        echo "[MQTT] Received: $payload"
        chip_id=$(echo "$payload" | jq -r '.id // empty')
        msg=$(echo "$payload" | jq -r '.message // empty')
        volt=$(echo "$payload" | jq -r '.volt // empty')
        raw=$(echo "$payload" | jq -r '.raw // empty')

        # Only handle messages that have chip id + volt, and no raw field
        if [[ -n "$chip_id" && -n "$msg" && -n "$volt" && -z "$raw" ]]; then
            for uid in "${!USERS[@]}"; do
                if [[ "${USERS[$uid]}" == "${USERS[1273591783]}" ]]; then
                    # Admin (Aditya) sees full info
                    send_telegram_message "$uid" "📡 Message received from chip: $chip_id, Message: $msg, Battery: $volt"
                else
                    # Others just see simplified notification
                    send_telegram_message "$uid" "❤️ Miss you"
                fi
            done
        else
            echo "[MQTT] Ignored: missing chipmac/volt or has raw field"
        fi
        # send_telegram_message "📡 MQTT Alert\nTopic: $MQTT_TOPIC\nMessage: $payload"
    done
}

telegram_poller() {
    echo "[INFO] Starting Telegram -> MQTT poller..."
    echo "[INFO] Initial OFFSET: $OFFSET (ignoring old messages)"

    while true; do
        response=$(curl -s "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/getUpdates?offset=${OFFSET}&timeout=30")
        updates=$(echo "$response" | jq -c '.result[]' 2>/dev/null)

        if [ -n "$updates" ]; then
            latest_update_id=$OFFSET

            while IFS= read -r update; do
                update_id=$(echo "$update" | jq -r '.update_id')
                message_text=$(echo "$update" | jq -r '.message.text // empty')
                chat_id=$(echo "$update" | jq -r '.message.chat.id')
                user_first=$(echo "$update" | jq -r '.message.from.first_name // ""')
                user_last=$(echo "$update" | jq -r '.message.from.last_name // ""')
                username=$(echo "$update" | jq -r '.message.from.username // ""')
                user_id=$(echo "$update" | jq -r '.message.from.id')

                if [ -n "$message_text" ]; then
                    echo "[TELEGRAM] From: ${user_first} ${user_last} (@${username}, ID: ${user_id})"
                    echo "[TELEGRAM] Message: $message_text"
                    lower_msg=$(echo "$message_text" | tr '[:upper:]' '[:lower:]')

                    if [[ "$lower_msg" =~ ^miss ]]; then
                        json_payload=$(jq -nc \
                            --arg id "$user_first" \
                            --arg message "$MQTT_MSG_1" \
                            --arg raw "$message_text" \
                            --arg user "$user_id" \
                            '{id:$id, message:$message, raw:$raw, user:$user}')

                        publish_mqtt_message "$MQTT_TOPIC" "$json_payload"
                        # Notify all users
                        for uid in "${!USERS[@]}"; do
                           if [[ "$uid" == "$user_id" ]]; then
                                # Sender gets confirmation
                                for rid in "${!USERS[@]}"; do
                                    if [[ "$rid" != "$user_id" ]]; then
                                        send_telegram_message "$uid" "❤️📡 Message sent to ${USERS[$rid]}"
                                    fi
                                done
                            else
                                # Receiver gets info
                                send_telegram_message "$uid" "❤️ ${USERS[$user_id]} is missing you"
                            fi
                        done
                    fi
                fi

                # Track the highest update_id seen
                latest_update_id=$update_id
            done <<< "$updates"

            # Move OFFSET forward once per batch
            OFFSET=$((latest_update_id + 1))
        fi

        sleep "$POLL_INTERVAL"
    done
}

# === INIT OFFSET (skip old messages) ===
echo "[INFO] Fetching last Telegram update to skip old messages..."
last_update=$(curl -s "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/getUpdates" | jq -r '.result[-1].update_id // 0')
if [ "$last_update" != "null" ] && [ -n "$last_update" ]; then
    OFFSET=$((last_update + 1))
else
    OFFSET=0
fi

# === START ===
mqtt_listener &
telegram_poller