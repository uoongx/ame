package com.ame.memory;

/** Normal chat uses the shared conversation flow without a guide prompt. */
public class ChatActivity extends BaseConversationActivity {
    @Override
    protected int conversationLayout() {
        return R.layout.activity_chat;
    }
}
